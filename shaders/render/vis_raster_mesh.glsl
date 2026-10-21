// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's hardware raster, mesh-shader path (docs/render.md
// "Rasterisation"; the mesh shader pipeline of §8.6.3): one workgroup per
// visible cluster of the phase's raster list, outputting the meshlet's
// vertices in clip space and its triangles with gl_PrimitiveID = cluster
// << 7 | triangle, which the fragment stage writes into the visibility
// buffer. Back faces and triangles that cover no pixel centre are culled
// here (vis_cull_triangle). Included by vis_raster_opaque.mesh and
// vis_raster_masked.mesh with VIS_MASKED 0 or 1; the masked variant also
// outputs what the alpha test needs: the UV and vertex alpha, and the
// material's texture, alpha factor and cutoff, read once a cluster here and
// handed on flat, so that the fragments (foliage draws many layers of them)
// read no record before their texture fetch: Bistro's hedge view rasterises
// in 1.85 ms instead of 2.17.
#extension GL_EXT_mesh_shader : require
#include "vis_common.glsl"

#ifndef VIS_MASKED
#error "include through vis_raster_opaque.mesh or vis_raster_masked.mesh"
#endif

// 32 invocations: the RTX 3060 Ti's preferred mesh workgroup size; a
// meshlet's 64 vertices and 124 triangles take two and four rounds.
layout(local_size_x = 32) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;
layout(push_constant, scalar) uniform Push { NphVisPush pc; };

#if VIS_MASKED
layout(location = 0) out vec3 out_uv_alpha[]; // UV, vertex-colour alpha
// The material's alpha test, read here once a cluster so that no fragment
// reads the records: the base colour texture and sampler
// (NPH_RENDER_NO_TEXTURE: none), and its alpha factor and cutoff.
layout(location = 1) flat out uvec2 out_alpha_texture[];
layout(location = 2) flat out vec2 out_alpha_test[];
#endif

shared vec4 s_clip[64];

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    // The masked pipelines draw the skin's list too (NphVisPush.mode).
    const uint kind = VIS_MASKED != 0 ? pc.mode : uint(NPH_VIS_RASTER_OPAQUE);
    const uint wg = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
    if (wg >= VisCountersRef(view.v.counters).c.raster_count[pc.phase][kind]) {
        SetMeshOutputsEXT(0u, 0u);
        return;
    }
    const uint g = VisUintRef(view.v.raster_lists[pc.phase][kind]).v[wg];
    const NphVisCluster cluster = VisClustersRef(view.v.clusters).c[g];
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[cluster.instance];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uvec4 m = VisMeshletsRef(mesh.meshlets).m[cluster.meshlet];
    const uint vertex_count = m.z & 0xFFFFu;
    const uint triangle_count = m.z >> 16u;
    SetMeshOutputsEXT(vertex_count, triangle_count);

#if VIS_MASKED
    // Field by field, not a copy of the record (docs/build.md "Shaders").
    MaterialsRef materials = MaterialsRef(view.v.materials);
    const uvec2 alpha_texture = uvec2(materials.m[cluster.material].textures[NPH_RENDER_TEX_BASE_COLOR],
                                      materials.m[cluster.material].samplers[NPH_RENDER_TEX_BASE_COLOR]);
    const vec2 alpha_test =
        vec2(materials.m[cluster.material].base_color.a, materials.m[cluster.material].alpha_cutoff);
#endif
    const vec3 t =
        vis_relative_translation(inst.transform, view.v.camera_high.xyz, view.v.camera_low.xyz);
    for (uint i = gl_LocalInvocationIndex; i < vertex_count; i += 32u) {
        const uint vertex = VisUintRef(mesh.meshlet_vertices).v[m.x + i];
        const vec3 p = vis_transform_vector(inst.transform, vis_position(mesh, vertex)) + t;
        const vec4 clip = view.v.view_proj * vec4(p, 1.0);
        gl_MeshVerticesEXT[i].gl_Position = clip;
        s_clip[i] = clip;
#if VIS_MASKED
        const uvec4 a = AttribsRef(mesh.attribs).a[vertex];
        out_uv_alpha[i] = vec3(unpackHalf2x16(a.z), unpackUnorm4x8(a.w).a);
        out_alpha_texture[i] = alpha_texture;
        out_alpha_test[i] = alpha_test;
#endif
    }
    barrier();

    const bool cull_back = (cluster.flags & NPH_VIS_CLUSTER_DOUBLE_SIDED) == 0u;
    const bool mirrored = (cluster.flags & NPH_VIS_CLUSTER_MIRRORED) != 0u;
    for (uint i = gl_LocalInvocationIndex; i < triangle_count; i += 32u) {
        const uvec3 c = vis_triangle_corners(mesh, m.y, i);
        gl_PrimitiveTriangleIndicesEXT[i] = c;
        gl_MeshPrimitivesEXT[i].gl_PrimitiveID = int((g << NPH_VIS_TRIANGLE_BITS) | i);
        gl_MeshPrimitivesEXT[i].gl_CullPrimitiveEXT =
            vis_cull_triangle(s_clip[c.x], s_clip[c.y], s_clip[c.z], cull_back, mirrored,
                              view.v.viewport);
    }
}

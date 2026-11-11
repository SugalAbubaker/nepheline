// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's page raster, mesh-shader path (vsm_raster.glsl):
// a workgroup per draw (instance, meshlet, page), outputting the meshlet's
// vertices in the page and every triangle (both sides cast). Included with
// VSM_MASKED set. 0 draws the casters whose material is not alpha tested,
// with no fragment stage: the depth test keeps the nearest. 1 draws those
// whose material is (the draw's fourth word, the material's index plus one:
// vsm_cull.comp), with what the alpha test needs for vsm_raster_masked.frag,
// as the visibility buffer's masked raster hands it on (vis_raster_mesh.glsl):
// a leaf's card casts its leaf's shadow, not its quad's. Both read the one
// list of draws, each passing the other's by with no output.
#extension GL_EXT_mesh_shader : require
#include "vsm_raster.glsl"

#ifndef VSM_MASKED
#error "include through a vsm_raster*.mesh wrapper"
#endif

layout(local_size_x = 32) in;
layout(triangles, max_vertices = 64, max_primitives = 124) out;
layout(push_constant, scalar) uniform Push { NphVisPush pc; };

out gl_MeshPerVertexEXT
{
    vec4 gl_Position;
    float gl_ClipDistance[4];
}
gl_MeshVerticesEXT[];

#if VSM_MASKED
layout(location = 0) out vec3 out_uv_alpha[]; // UV, vertex-colour alpha
// The material's alpha test, read here once a draw: the base colour texture
// and its sampler (NPH_RENDER_NO_TEXTURE: none), and its alpha factor and
// cutoff.
layout(location = 1) flat out uvec2 out_alpha_texture[];
layout(location = 2) flat out vec2 out_alpha_test[];
#endif

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    VsmViewRef r = VsmViewRef(ViewRef(view.v.render_view).v.vsm);
    const uint wg = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
    if (wg >= min(VsmCountersRef(r.v.counters).c.draw_count, r.v.max_draws)) {
        SetMeshOutputsEXT(0u, 0u);
        return;
    }
    const uvec4 item = VsmUvec4Ref(r.v.draws).v[wg];
    if ((item.w != 0u) != (VSM_MASKED != 0)) {
        SetMeshOutputsEXT(0u, 0u); // the other pipeline's
        return;
    }
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[item.x];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uvec4 m = VisMeshletsRef(mesh.meshlets).m[item.y];
    const uint vertex_count = m.z & 0xFFFFu;
    const uint triangle_count = m.z >> 16u;
    SetMeshOutputsEXT(vertex_count, triangle_count);

#if VSM_MASKED
    // Field by field, not a copy of the record (docs/build.md "Shaders").
    MaterialsRef materials = MaterialsRef(view.v.materials);
    const uint material = item.w - 1u;
    const uvec2 alpha_texture = uvec2(materials.m[material].textures[NPH_RENDER_TEX_BASE_COLOR],
                                      materials.m[material].samplers[NPH_RENDER_TEX_BASE_COLOR]);
    const vec2 alpha_test =
        vec2(materials.m[material].base_color.a, materials.m[material].alpha_cutoff);
#endif
    const VsmTarget target = vsm_target(r, item.z);
    const vec2 extent = vsm_pool_extent(r);
    const vec3 t =
        vis_relative_translation(inst.transform, view.v.camera_high.xyz, view.v.camera_low.xyz);
    for (uint i = gl_LocalInvocationIndex; i < vertex_count; i += 32u) {
        const uint vertex = VisUintRef(mesh.meshlet_vertices).v[m.x + i];
        const vec3 p = vis_transform_vector(inst.transform, vis_position(mesh, vertex)) + t;
        vec4 clip;
        gl_MeshVerticesEXT[i].gl_Position = vsm_raster_position(r, target, extent, p, clip);
        gl_MeshVerticesEXT[i].gl_ClipDistance[0] = clip.x;
        gl_MeshVerticesEXT[i].gl_ClipDistance[1] = clip.y;
        gl_MeshVerticesEXT[i].gl_ClipDistance[2] = clip.z;
        gl_MeshVerticesEXT[i].gl_ClipDistance[3] = clip.w;
#if VSM_MASKED
        const uvec4 a = AttribsRef(mesh.attribs).a[vertex];
        out_uv_alpha[i] = vec3(unpackHalf2x16(a.z), unpackUnorm4x8(a.w).a);
        out_alpha_texture[i] = alpha_texture;
        out_alpha_test[i] = alpha_test;
#endif
    }
    for (uint i = gl_LocalInvocationIndex; i < triangle_count; i += 32u) {
        gl_PrimitiveTriangleIndicesEXT[i] = vis_triangle_corners(mesh, m.y, i);
    }
}

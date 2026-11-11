// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's page raster, indirect-draw path (vsm_raster.glsl):
// one indexed draw per (instance, meshlet, page) over the meshlet index pool
// (vis_raster.vert's addressing), its record found by gl_DrawID. Included
// with VSM_MASKED set. 0 draws the casters whose material is not alpha
// tested, with no fragment stage; 1 those whose material is (the draw's
// fourth word, the material's index plus one), with what the alpha test
// needs for vsm_raster_masked.frag. Each has its own indirect commands over
// the one list of draws, the other's with no indices (vsm_cull.comp).
#include "vsm_raster.glsl"

#ifndef VSM_MASKED
#error "include through a vsm_raster*.vert wrapper"
#endif

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

out gl_PerVertex
{
    vec4 gl_Position;
    float gl_ClipDistance[4];
};

#if VSM_MASKED
layout(location = 0) out vec3 out_uv_alpha; // UV, vertex-colour alpha
layout(location = 1) flat out uvec2 out_alpha_texture;
layout(location = 2) flat out vec2 out_alpha_test;
#endif

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    VsmViewRef r = VsmViewRef(ViewRef(view.v.render_view).v.vsm);
    const uvec4 item = VsmUvec4Ref(r.v.draws).v[gl_DrawID];
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[item.x];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uint vertex = VisUintRef(mesh.meshlet_vertices).v[gl_VertexIndex];
    const vec3 t =
        vis_relative_translation(inst.transform, view.v.camera_high.xyz, view.v.camera_low.xyz);
    const vec3 p = vis_transform_vector(inst.transform, vis_position(mesh, vertex)) + t;
    vec4 clip;
    gl_Position = vsm_raster_position(r, vsm_target(r, item.z), vsm_pool_extent(r), p, clip);
    gl_ClipDistance[0] = clip.x;
    gl_ClipDistance[1] = clip.y;
    gl_ClipDistance[2] = clip.z;
    gl_ClipDistance[3] = clip.w;
#if VSM_MASKED
    // Field by field, not a copy of the record (docs/build.md "Shaders").
    MaterialsRef materials = MaterialsRef(view.v.materials);
    const uint material = item.w - 1u;
    const uvec4 a = AttribsRef(mesh.attribs).a[vertex];
    out_uv_alpha = vec3(unpackHalf2x16(a.z), unpackUnorm4x8(a.w).a);
    out_alpha_texture = uvec2(materials.m[material].textures[NPH_RENDER_TEX_BASE_COLOR],
                              materials.m[material].samplers[NPH_RENDER_TEX_BASE_COLOR]);
    out_alpha_test = vec2(materials.m[material].base_color.a, materials.m[material].alpha_cutoff);
#endif
}

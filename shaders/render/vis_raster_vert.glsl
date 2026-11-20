// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's hardware raster, indirect-draw path for devices
// without mesh shaders (docs/render.md "Rasterisation"; §8.6.3 "draws are
// generated with vkCmdDrawIndexedIndirectCount"): one indexed draw per
// visible cluster, whose indices are the meshlet's local corners in the
// renderer's index pool and whose vertex offset is the meshlet's first
// entry in its mesh's meshlet-vertex array, so gl_VertexIndex addresses
// that array. gl_DrawID is the draw's place in the phase's raster list,
// which names the cluster. Included by vis_raster_opaque.vert and
// vis_raster_masked.vert with VIS_MASKED 0 or 1.
#include "vis_common.glsl"

#ifndef VIS_MASKED
#error "include through vis_raster_opaque.vert or vis_raster_masked.vert"
#endif

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

layout(location = 0) flat out uint out_cluster;
#if VIS_MASKED
layout(location = 1) out vec3 out_uv_alpha; // UV, vertex-colour alpha
#endif

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    const uint kind = uint(VIS_MASKED);
    const uint g = VisUintRef(view.v.raster_lists[pc.phase][kind]).v[gl_DrawID];
    const NphVisCluster cluster = VisClustersRef(view.v.clusters).c[g];
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[cluster.instance];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[cluster.mesh];
    const uint vertex = VisUintRef(mesh.meshlet_vertices).v[gl_VertexIndex];
    const vec3 t =
        vis_relative_translation(inst.transform, view.v.camera_high.xyz, view.v.camera_low.xyz);
    const vec3 p = vis_transform_vector(inst.transform, vis_position(mesh, vertex)) + t;
    gl_Position = view.v.view_proj * vec4(p, 1.0);
    out_cluster = g;
#if VIS_MASKED
    const uvec4 a = AttribsRef(mesh.attribs).a[vertex];
    // The material's coordinates (ADR-0105), field by field (docs/build.md "Shaders").
    MaterialsRef materials = MaterialsRef(view.v.materials);
    vec2 uv = unpackHalf2x16(a.z);
    // The base colour's set (ADR-0107).
    if (nph_material_slot_uv1(materials.m[cluster.material].wrap, NPH_RENDER_TEX_BASE_COLOR)) {
        uv = nph_uv1(mesh.uv1, vertex, uv);
    }
    if (nph_material_has_uv(materials.m[cluster.material].flags)) {
        uv = nph_material_uv(materials.m[cluster.material].uv_u, materials.m[cluster.material].uv_v,
                             uv);
    }
    out_uv_alpha = vec3(uv, unpackUnorm4x8(a.w).a);
#endif
}

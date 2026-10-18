// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's page raster, indirect-draw path for devices
// without mesh shaders (vsm_raster.glsl): one indexed draw per (instance,
// meshlet, page) over the index pool, as the visibility buffer's indirect
// raster draws (vis_raster_vert.glsl); gl_DrawID names the draw.
#version 460
#include "vsm_raster.glsl"

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

out gl_PerVertex
{
    vec4 gl_Position;
    float gl_ClipDistance[4];
};

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
}

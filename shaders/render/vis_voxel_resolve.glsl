// SPDX-License-Identifier: Apache-2.0
// The voxel resolve (docs/render.md "Voxels"; ADR-0061): the pixels of voxel
// clusters, which the triangle resolve (vis_resolve.glsl) leaves unwritten,
// shaded in the tiles it lists for them, a workgroup a tile through an
// indirect dispatch. The two compile separately, so the triangles' resolve
// keeps its registers (docs/render.md "Shading").
//
// A pixel's voxel is found again by the raster's own traversal of the brick
// the visibility buffer names (vis_voxel.glsl vis_voxel_surface): the voxel,
// the point, and the face of its footprint the ray entered, the geometric
// normal. The point's shadow lookups, shadow rays, and radiance-cache cell
// start a cell's diagonal off that face, clear of the triangles the voxel
// stands for, which the shadow map and the rays still draw. It is shaded
// with surface.glsl's model integrated over the voxel's lobe of normals
// (nph_surface_shade_voxel), with the lights, the sun's virtual shadow map,
// the radiance cache, and the ambient occlusion as the triangles have them.
//
// Compiled twice, as the triangle resolve is (vis_voxel_resolve.comp, and
// vis_voxel_resolve_rt.comp with ray queries), and specialised alike: the
// HDR target's dither (k_dither), the virtual shadow map (k_vsm), the
// radiance cache (k_gi), the debug views (k_debug). Its push constants are
// the resolve's (NphVisResolvePush).
#ifndef NPH_VIS_VOXEL_RESOLVE_GLSL
#define NPH_VIS_VOXEL_RESOLVE_GLSL

#include "vis_voxel.glsl"
#include "nph_bindless_int64.glsl"

#ifdef NPH_RAY_QUERY
#include "rt_common.glsl"
uint64_t g_tlas;
vec3 g_tlas_offset;
#define NPH_SURFACE_SHADOW(p, l, distance)                                                         \
    nph_shadow_ray(g_tlas, (p).position + g_tlas_offset, (p).face_normal, (l), (distance))
#endif

#define NPH_VSM_SAMPLING
#include "vsm_common.glsl"
layout(constant_id = 1) const bool k_vsm = false;
VsmViewRef g_vsm; // set first thing in main()
#define NPH_SURFACE_VSM_ACTIVE k_vsm
#define NPH_SURFACE_VSM(p)     vsm_shadow(g_vsm, (p).position, (p).face_normal, (p).n)

ivec2 g_pixel;
uint g_ao; // NPH_VIS_NO_AO: none

float vis_ambient_occlusion()
{
    return texelFetch(nph_textures_2d[g_ao], g_pixel, 0).r;
}
#define NPH_SURFACE_AO (g_ao != NPH_VIS_NO_AO ? vis_ambient_occlusion() : 1.0)

#include "gi_common.glsl"
layout(constant_id = 2) const bool k_gi = false;
vec4 g_gi_cell; // set in main() where k_gi
#define NPH_SURFACE_GI_ACTIVE k_gi
#define NPH_SURFACE_GI        g_gi_cell

// A voxel samples no texture.
#define NPH_SURFACE_SAMPLE(tex, smp, s) vec4(1.0)
#define NPH_SURFACE_GRID_LIGHTS
#define NPH_SURFACE_VOXEL
layout(constant_id = 3) const bool k_debug = false;
#define NPH_SURFACE_DEBUG_MODE(view) (k_debug ? (view).v.debug_mode : NPH_RENDER_DEBUG_NONE)
#include "surface.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant, scalar) uniform Push { NphVisResolvePush pc; };

// Whether the HDR target is B10G11R11, whose coarse steps are dithered.
layout(constant_id = 0) const bool k_dither = false;

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    const uint wg = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
    if (wg >= VisCountersRef(view.v.counters).c.voxel_tiles) {
        return;
    }
    const uint tile = VisUintRef(view.v.voxel_tiles).v[wg];
    const ivec2 p = ivec2(int(tile & 0xFFFFu), int(tile >> 16u)) * NPH_VIS_RESOLVE_TILE +
                    ivec2(gl_LocalInvocationID.xy);
    if (p.x >= int(pc.size & 0xFFFFu) || p.y >= int(pc.size >> 16u)) {
        return;
    }
    const uint64_t v = imageLoad(nph_u64images_2d[nonuniformEXT(pc.vis_index)], p).x;
    if (v == 0ul ||
        (VisClustersRef(pc.clusters).c[vis_cluster(v)].flags & NPH_VIS_CLUSTER_VOXELS) == 0u) {
        return; // the triangle resolve's pixel
    }
    ViewRef rview = ViewRef(pc.render_view);
#ifdef NPH_RAY_QUERY
    g_tlas = rview.v.tlas;
    g_tlas_offset = rview.v.tlas_offset.xyz;
#endif
    g_vsm = VsmViewRef(rview.v.vsm);
    g_pixel = p;
    g_ao = pc.ao_index;
    const uint debug = NPH_SURFACE_DEBUG_MODE(rview);
    const bool dither = k_dither && debug == NPH_RENDER_DEBUG_NONE;

    VisVoxelSurface s;
    vis_voxel_surface(view, rview.v.inv_view_proj, v, p, s);
    // Where its rays and lookups start: a cell's diagonal off the face.
    const vec3 lit = s.position + s.face_normal * s.offset;
    if (k_gi) {
        g_gi_cell = gi_surface_cell(GiViewRef(rview.v.gi), p, lit, s.face_normal);
    }
    const uvec4 voxel = s.voxel;
    const vec3 albedo = nph_srgb_to_linear(unpackUnorm4x8(voxel.x).rgb);
    const float occlusion = float(voxel.x >> 24u) / 255.0;
    const vec3 axis = normalize(vis_normal_matrix(s.transform) *
                                nph_oct_decode(unpackSnorm2x16(voxel.y)));
    const float roughness = float(voxel.z & 0xFFu) / 255.0;
    const float metallic = float((voxel.z >> 8u) & 0xFFu) / 255.0;
    const float kappa = vis_voxel_kappa((voxel.z >> 16u) & 0xFFu);
    const vec3 color = nph_surface_shade_voxel(rview, albedo, roughness, metallic, occlusion, axis,
                                               kappa, lit, s.face_normal, vec2(p) + 0.5,
                                               vis_depth(v), debug == NPH_RENDER_DEBUG_IRRADIANCE);
    vec3 stored = color;
    if (dither) {
        stored = nph_dither_b10g11r11(color, nph_gradient_noise(vec2(p)));
    }
    imageStore(nph_images_2d[nonuniformEXT(pc.hdr_index)], p, vec4(stored, 1.0));
}

#endif

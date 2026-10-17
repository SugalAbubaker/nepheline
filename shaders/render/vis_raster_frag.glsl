// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's hardware raster, fragment stage (docs/render.md
// "Rasterisation"): no attachments at all; each fragment writes its depth
// and its cluster and triangle into the 64-bit visibility buffer with one
// atomic max, which keeps the nearest surface because the depth is the
// high half and reverse-Z makes nearer larger (Karis et al., "Nanite: A
// Deep Dive", SIGGRAPH 2021). The software rasteriser writes the same
// buffer the same way.
//
// Back faces of single-sided materials are discarded (the mesh path culls
// most already; triangles reaching behind the camera get here), and the
// masked variant runs the material's alpha test: the base colour's alpha
// (factor, vertex colour, texture) against the cutoff, as the reference
// renderer's MASK does. Included with VIS_MESH (0: the vertex path) and
// VIS_MASKED set. arg0 is the visibility buffer's storage index.
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"

#if !defined(VIS_MESH) || !defined(VIS_MASKED)
#error "include through a vis_raster_*.frag wrapper"
#endif

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

#if !VIS_MESH
layout(location = 0) flat in uint in_cluster;
#endif
#if VIS_MASKED
#if VIS_MESH
layout(location = 0) in vec3 in_uv_alpha;
#else
layout(location = 1) in vec3 in_uv_alpha;
#endif
#endif

void main()
{
    VisViewRef view = VisViewRef(pc.view);
#if VIS_MESH
    const uint id = uint(gl_PrimitiveID);
    const uint g = id >> NPH_VIS_TRIANGLE_BITS;
    const uint triangle = id & NPH_VIS_TRIANGLE_MASK;
#else
    const uint g = in_cluster;
    const uint triangle = uint(gl_PrimitiveID);
#endif
    const NphVisCluster cluster = VisClustersRef(view.v.clusters).c[g];
    const bool front = gl_FrontFacing != ((cluster.flags & NPH_VIS_CLUSTER_MIRRORED) != 0u);
    bool keep = front || (cluster.flags & NPH_VIS_CLUSTER_DOUBLE_SIDED) != 0u;
#if VIS_MASKED
    // Sampled before any discard so the quad's derivatives stay defined.
    const NphRenderMaterialData md = MaterialsRef(view.v.materials).m[cluster.material];
    float alpha = md.base_color.a * in_uv_alpha.z;
    if (md.textures[NPH_RENDER_TEX_BASE_COLOR] != NPH_RENDER_NO_TEXTURE) {
        alpha *= nph_sample_2d(md.textures[NPH_RENDER_TEX_BASE_COLOR],
                               md.samplers[NPH_RENDER_TEX_BASE_COLOR], in_uv_alpha.xy)
                     .a;
    }
    keep = keep && !(alpha < md.alpha_cutoff);
#endif
    if (!keep) {
        discard;
    }
    imageAtomicMax(nph_u64images_2d[nonuniformEXT(pc.arg0)], ivec2(gl_FragCoord.xy),
                   vis_pack(gl_FragCoord.z, g, triangle));
}

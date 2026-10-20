// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's hardware raster, fragment stage (docs/render.md
// "Rasterisation"): no colour attachments; each fragment writes its depth
// and its cluster and triangle into the 64-bit visibility buffer with one
// atomic max, which keeps the nearest surface because the depth is the
// high half and reverse-Z makes nearer larger (Karis et al., "Nanite: A
// Deep Dive", SIGGRAPH 2021). The software rasteriser writes the same
// buffer the same way.
//
// The mesh path culls every back face of a single-sided material per
// triangle (vis_cull_triangle), and runs the depth test early against the
// raster's depth attachment: a fragment behind what the pass already drew
// is rejected before the shader and its atomic run (the atomic still
// decides, the test passes equal depths). Its opaque variant never
// discards and writes depth. Its masked variant runs the material's alpha
// test, the base colour's alpha (factor, vertex colour, texture) against
// the cutoff as the reference renderer's MASK does, so it writes no depth
// (an early write would let a discarded fragment occlude): it is tested
// against the opaque surfaces drawn before it, and its texture is not
// sampled behind them. The vertex path has no depth attachment; it
// discards back faces here, and runs the same alpha test in its masked
// variant. Included with VIS_MESH (0: the vertex path) and VIS_MASKED set.
// arg0 is the visibility buffer's storage index.
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"

#if !defined(VIS_MESH) || !defined(VIS_MASKED)
#error "include through a vis_raster_*.frag wrapper"
#endif

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

#if VIS_MESH
layout(early_fragment_tests) in;
#endif

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
#if !VIS_MESH || VIS_MASKED
    const NphVisCluster cluster = VisClustersRef(view.v.clusters).c[g];
#if VIS_MESH
    bool keep = true; // back faces culled per triangle
#else
    const bool front = gl_FrontFacing != ((cluster.flags & NPH_VIS_CLUSTER_MIRRORED) != 0u);
    bool keep = front || (cluster.flags & NPH_VIS_CLUSTER_DOUBLE_SIDED) != 0u;
#endif
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
#endif
    imageAtomicMax(nph_u64images_2d[nonuniformEXT(pc.arg0)], ivec2(gl_FragCoord.xy),
                   vis_pack(gl_FragCoord.z, g, triangle));
}

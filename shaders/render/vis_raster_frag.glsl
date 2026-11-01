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
// test (most small masked clusters take it in the software raster instead,
// vis_sw_raster.glsl), the base colour's alpha (factor, vertex colour, texture)
// against the cutoff as the reference renderer's MASK does, so it writes no depth
// (an early write would let a discarded fragment occlude): it is tested
// against the opaque surfaces drawn before it, and its texture is not
// sampled behind them. The mesh stage hands it the material's alpha test
// (vis_raster_mesh.glsl), so the texture fetch is its only read before the
// atomic. The masked skin (VIS_LATE_Z: the masked clusters at the front of
// their instance, vis_cull_meshlets.comp) runs the same alpha test without
// the early test and writes its depth after it, before the other masked
// clusters, so that their fragments behind a leaf the skin drew are
// rejected before their shader; the depth it writes is that of a fragment
// that passed its alpha test, which no farther fragment could beat in the
// atomic. The vertex path has no depth attachment; it
// discards back faces here, and runs the same alpha test in its masked
// variant. Included with VIS_MESH (0: the vertex path) and VIS_MASKED set.
// arg0 is the visibility buffer's storage index.
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"

#if !defined(VIS_MESH) || !defined(VIS_MASKED)
#error "include through a vis_raster_*.frag wrapper"
#endif
// The ROP variant (phase 0's opaque clusters on the mesh path): the depth
// and the cluster and triangle as the visibility buffer's two 32-bit words,
// written through its colour-attachment view, where the early depth test
// with writes leaves the nearest fragment's.
#ifndef VIS_ROP
#define VIS_ROP 0
#endif
#if VIS_ROP
layout(location = 0) out uvec2 out_visibility;
#endif

layout(push_constant, scalar) uniform Push { NphVisPush pc; };

// The masked skin (VIS_LATE_Z) writes its depth after the alpha test, so it
// takes no early test.
#ifndef VIS_LATE_Z
#define VIS_LATE_Z 0
#endif
#if VIS_MESH && !VIS_LATE_Z
layout(early_fragment_tests) in;
#endif

#if !VIS_MESH
layout(location = 0) flat in uint in_cluster;
#endif
#if VIS_MASKED
#if VIS_MESH
layout(location = 0) in vec3 in_uv_alpha;
// The material's alpha test from the mesh stage (vis_raster_mesh.glsl).
layout(location = 1) flat in uvec2 in_alpha_texture;
layout(location = 2) flat in vec2 in_alpha_test;
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
#if VIS_MESH
    bool keep = true; // back faces culled per triangle
#else
    const NphVisCluster cluster = VisClustersRef(view.v.clusters).c[g];
    const bool front = gl_FrontFacing != ((cluster.flags & NPH_VIS_CLUSTER_MIRRORED) != 0u);
    bool keep = front || (cluster.flags & NPH_VIS_CLUSTER_DOUBLE_SIDED) != 0u;
#endif
#if VIS_MASKED
    // Sampled before any discard so the quad's derivatives stay defined.
#if VIS_MESH
    const uvec2 alpha_texture = in_alpha_texture;
    const vec2 alpha_test = in_alpha_test;
#else
    const NphRenderMaterialData md = MaterialsRef(view.v.materials).m[cluster.material];
    const uvec2 alpha_texture =
        uvec2(md.textures[NPH_RENDER_TEX_BASE_COLOR], md.samplers[NPH_RENDER_TEX_BASE_COLOR]);
    const vec2 alpha_test = vec2(md.base_color.a, md.alpha_cutoff);
#endif
    float alpha = alpha_test.x * in_uv_alpha.z;
    if (alpha_texture.x != NPH_RENDER_NO_TEXTURE) {
        alpha *= nph_sample_2d(alpha_texture.x, alpha_texture.y, in_uv_alpha.xy).a;
    }
    keep = keep && !(alpha < alpha_test.y);
#endif
    if (!keep) {
        discard;
    }
#endif
#if VIS_ROP
    out_visibility = uvec2((g << NPH_VIS_TRIANGLE_BITS) | triangle, floatBitsToUint(gl_FragCoord.z));
#else
    imageAtomicMax(nph_u64images_2d[nonuniformEXT(pc.arg0)], ivec2(gl_FragCoord.xy),
                   vis_pack(gl_FragCoord.z, g, triangle));
#endif
}

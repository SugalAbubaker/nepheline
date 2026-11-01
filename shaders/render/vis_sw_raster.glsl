// SPDX-License-Identifier: Apache-2.0
// The visibility buffer's software raster (docs/render.md "The production
// path"; CLAUDE.md §8.6.4, "micro-triangles use a compute software
// rasterizer with 64-bit image atomics"; Karis et al., "Nanite: A Deep
// Dive", SIGGRAPH 2021): one workgroup per cluster of the phase's software
// list, which the meshlet cull fills with clusters of small triangles.
//
// The cluster's vertices go to screen space once, into shared memory,
// snapped to 1/256 pixel as the hardware snaps them (Vulkan's
// subPixelPrecisionBits: 8 on the RTX 3060 Ti and on lavapipe). Then each
// invocation takes a triangle, culls back faces as the hardware path does,
// and walks the pixel centres of its bounding box with exact integer edge
// functions under the top-left rule (Pineda, "A Parallel Algorithm for
// Polygon Rasterization", SIGGRAPH 1988; the tie-breaking rule of Direct3D):
// a centre on an edge belongs to the triangle the edge is a top or left
// edge of, so neighbouring triangles, whichever raster draws them, cover
// every centre once. Each covered pixel takes the atomic max of (depth,
// cluster, triangle) the hardware path's fragment shader writes; the depth
// is the vertices' z/w interpolated in screen space, as the rasteriser
// interpolates it.
//
// The meshlet cull sends a cluster here only when its bounding sphere
// projects in front of the near plane into at most NPH_VIS_SW_MAX_PIXELS
// across and its triangles average at most sw_raster_pixels across: every
// vertex has w > 0 (nothing to clip), every triangle's box is small, and a
// thread's walk over it is short.
//
// Masked clusters (VIS_SW_MASKED) have a pipeline and a list of their own, since
// the same code as a branch a cluster made the opaque clusters' pass 7% slower
// (ADR-0071). Each takes the material's alpha test at each covered pixel, read
// once a cluster as the mesh stage reads it (vis_raster_mesh.glsl), with the
// derivatives of its triangle's UV plane where the hardware path takes them
// across a quad: 2 x 2 quads of fragments and the helpers' fetches are most of
// what a triangle of a pixel costs there. Its UV is interpolated in screen
// space: the cull's bound keeps a cluster's w within about 2% of its middle, so
// that perspective moves it by less than 2% of a small triangle's UV span, and
// the correction costs 2% of the pass.
//
// arg0 is the visibility buffer's storage index. Included with VIS_SW_MASKED set.
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"

// 32 invocations: a cluster's 64 vertices and 124 triangles take two and four
// rounds, and the many clusters of a dozen triangles leave fewer idle than 64
// would (measured with render_perf on the RTX 3060 Ti).
layout(local_size_x = 32) in;
layout(push_constant, scalar) uniform Push { NphVisPush pc; };

shared ivec2 s_xy[64]; // screen position in 1/256 pixel
shared float s_z[64];  // depth, z / w
#if VIS_SW_MASKED
shared vec3 s_uv_alpha[64]; // UV, vertex-colour alpha
#endif

// The edge function of the edge u -> v at p, in 1/256-pixel units squared:
// positive on the side the triangle's inside is on once the triangle is
// wound so that its area is positive. The coordinates are relative to the
// triangle's box, which lies inside a cluster at most
// NPH_VIS_SW_MAX_PIXELS across, so every difference fits 15 bits and every
// product 30: 32-bit integers are exact (64-bit integer arithmetic is
// emulated on the reference GPU).
int edge(ivec2 u, ivec2 v, ivec2 p)
{
    return (v.x - u.x) * (p.y - u.y) - (v.y - u.y) * (p.x - u.x);
}

// The top-left rule with y down and the inside on the positive side: a
// horizontal edge running right is a top edge, an edge running up a left
// edge. A centre on any other edge is outside, which subtracting one from
// the integer edge function decides.
int bias(ivec2 u, ivec2 v)
{
    const ivec2 d = v - u;
    return (d.y < 0 || (d.y == 0 && d.x > 0)) ? 0 : -1;
}

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    const uint kind =
        VIS_SW_MASKED != 0 ? uint(NPH_VIS_RASTER_SOFTWARE_MASKED) : uint(NPH_VIS_RASTER_SOFTWARE);
    const uint wg = gl_WorkGroupID.x + gl_WorkGroupID.y * gl_NumWorkGroups.x;
    // The same for the whole workgroup, so no invocation misses the barrier.
    if (wg >= VisCountersRef(view.v.counters).c.raster_count[pc.phase][kind]) {
        return;
    }
    const uint g = VisUintRef(view.v.raster_lists[pc.phase][kind]).v[wg];
    const NphVisCluster cluster = VisClustersRef(view.v.clusters).c[g];
    const NphVisInstance inst = VisInstancesRef(view.v.instances).i[cluster.instance];
    const NphVisMesh mesh = VisMeshesRef(view.v.meshes).m[inst.mesh];
    const uvec4 m = VisMeshletsRef(mesh.meshlets).m[cluster.meshlet];
    const uint vertex_count = m.z & 0xFFFFu;
    const uint triangle_count = m.z >> 16u;
#if VIS_SW_MASKED
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
        // The viewport transform of Vulkan ("Coordinate Transformations"),
        // then the snap.
        const vec2 screen = (clip.xy / clip.w * 0.5 + 0.5) * view.v.viewport.xy;
        s_xy[i] = ivec2(roundEven(screen * 256.0));
        s_z[i] = clip.z / clip.w;
#if VIS_SW_MASKED
        const uvec4 attrib = AttribsRef(mesh.attribs).a[vertex];
        s_uv_alpha[i] = vec3(unpackHalf2x16(attrib.z), unpackUnorm4x8(attrib.w).a);
#endif
    }
    barrier();

    const bool cull_back = (cluster.flags & NPH_VIS_CLUSTER_DOUBLE_SIDED) == 0u;
    const bool mirrored = (cluster.flags & NPH_VIS_CLUSTER_MIRRORED) != 0u;
    const ivec2 last = ivec2(view.v.viewport.xy) - 1;
    for (uint tri = gl_LocalInvocationIndex; tri < triangle_count; tri += 32u) {
        const uvec3 corner = vis_triangle_corners(mesh, m.y, tri);
        const ivec2 a = s_xy[corner.x];
        const ivec2 b = s_xy[corner.y];
        const ivec2 c = s_xy[corner.z];
        // The box first: everything after is relative to its corner.
        const ivec2 lo = min(min(a, b), c);
        const ivec2 hi = max(max(a, b), c);
        // The pixels whose centres (x + 1/2, y + 1/2) the box holds.
        const ivec2 p0 = max((lo + 127) >> 8, ivec2(0));
        const ivec2 p1 = min((hi - 128) >> 8, last);
        const ivec2 ra = a - lo;
        ivec2 rb = b - lo;
        ivec2 rc = c - lo;
        const float za = s_z[corner.x];
        float zb = s_z[corner.y];
        float zc = s_z[corner.z];
#if VIS_SW_MASKED
        const vec3 ua = s_uv_alpha[corner.x];
        vec3 ub = s_uv_alpha[corner.y];
        vec3 uc = s_uv_alpha[corner.z];
#endif
        int area = edge(ra, rb, rc);
        if (area == 0) {
            continue;
        }
        // A negative area in framebuffer coordinates is counter-clockwise
        // on screen, the engine's front face (vis_front_facing's
        // determinant has the same sign when every w is positive).
        const bool front = (area < 0) != mirrored;
        if ((cull_back && !front) || any(greaterThan(p0, p1))) {
            continue;
        }
        if (area < 0) {
            const ivec2 tb = rb;
            rb = rc;
            rc = tb;
            const float tz = zb;
            zb = zc;
            zc = tz;
#if VIS_SW_MASKED
            const vec3 tu = ub;
            ub = uc;
            uc = tu;
#endif
            area = -area;
        }
        const int bias_a = bias(rb, rc);
        const int bias_b = bias(rc, ra);
        const int bias_c = bias(ra, rb);
        const float inv_area = 1.0 / float(area);
#if VIS_SW_MASKED
        // The UV's plane through the three vertices, per pixel: the
        // derivatives the hardware takes across a quad, which perspective
        // hardly moves across a triangle this small.
        const vec2 e1 = vec2(rb - ra);
        const vec2 e2 = vec2(rc - ra);
        const vec2 d1 = ub.xy - ua.xy;
        const vec2 d2 = uc.xy - ua.xy;
        const float per_pixel = 256.0 * inv_area;
        const vec2 ddx_uv = (d1 * e2.y - d2 * e1.y) * per_pixel;
        const vec2 ddy_uv = (d2 * e1.x - d1 * e2.x) * per_pixel;
#endif
        // The edge functions at the first centre, and their steps per pixel.
        const ivec2 start = p0 * 256 + 128 - lo;
        int row_a = edge(rb, rc, start);
        int row_b = edge(rc, ra, start);
        int row_c = edge(ra, rb, start);
        const int dx_a = (rb.y - rc.y) * 256;
        const int dx_b = (rc.y - ra.y) * 256;
        const int dx_c = (ra.y - rb.y) * 256;
        const int dy_a = (rc.x - rb.x) * 256;
        const int dy_b = (ra.x - rc.x) * 256;
        const int dy_c = (rb.x - ra.x) * 256;
        for (int y = p0.y; y <= p1.y; ++y) {
            int ea = row_a;
            int eb = row_b;
            int ec = row_c;
            for (int x = p0.x; x <= p1.x; ++x) {
                // All three non-negative: no sign bit in their union.
                if (((ea + bias_a) | (eb + bias_b) | (ec + bias_c)) >= 0) {
                    // Barycentrics: each edge function over the area weighs
                    // the vertex opposite the edge.
#if VIS_SW_MASKED
                    const vec3 uva = (float(ea) * ua + float(eb) * ub + float(ec) * uc) * inv_area;
                    float alpha = alpha_test.x * uva.z;
                    if (alpha_texture.x != NPH_RENDER_NO_TEXTURE) {
                        alpha *= nph_sample_2d_grad(alpha_texture.x, alpha_texture.y, uva.xy, ddx_uv,
                                                    ddy_uv)
                                     .a;
                    }
                    if (!(alpha < alpha_test.y))
#endif
                    {
                        const float depth =
                            (float(ea) * za + float(eb) * zb + float(ec) * zc) * inv_area;
                        imageAtomicMax(nph_u64images_2d[nonuniformEXT(pc.arg0)], ivec2(x, y),
                                       vis_pack(depth, g, tri));
                    }
                }
                ea += dx_a;
                eb += dx_b;
                ec += dx_c;
            }
            row_a += dy_a;
            row_b += dy_b;
            row_c += dy_c;
        }
    }
}

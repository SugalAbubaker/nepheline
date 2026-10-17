// SPDX-License-Identifier: Apache-2.0
// Shared by the GPU-driven visibility-buffer renderer's shaders
// (shaders/render/vis_*; docs/render.md "The production path"): buffer
// references to the records of nph_vis.h, camera-relative transforms,
// meshlet decoding, the culling tests, and the visibility-buffer encoding.
// The culling tests have C twins in engine/render/src/reference.c
// (nph_render_ref_vis_*) that the render tests compare against.
#ifndef NPH_VIS_COMMON_GLSL
#define NPH_VIS_COMMON_GLSL

#include "render_common.glsl"
#include "nph_vis.h"

NPH_BUFFER_REF(VisViewRef) { NphVisViewData v; };
NPH_BUFFER_REF(VisInstancesRef) { NphVisInstance i[]; };
NPH_BUFFER_REF(VisTransformsRef) { NphVisTransform t[]; };
NPH_BUFFER_REF(VisMeshesRef) { NphVisMesh m[]; };
NPH_BUFFER_REF(VisCountersRef) { NphVisCounters c; };
NPH_BUFFER_REF(VisArgsRef) { NphVisArgs a; };
NPH_BUFFER_REF(VisClustersRef) { NphVisCluster c[]; };
NPH_BUFFER_REF(VisDrawsRef) { NphVisDraw d[]; };
NPH_BUFFER_REF(VisUintRef) { uint v[]; };
NPH_BUFFER_REF(VisUvec2Ref) { uvec2 v[]; };
// NphRenderMeshlet: x vertex_offset, y triangle_offset, z vertex_count |
// triangle_count << 16, w submesh.
NPH_BUFFER_REF(VisMeshletsRef) { uvec4 m[]; };
// NphRenderMeshletBounds as two uvec4 per meshlet: (centre, radius) and
// (cone axis and cutoff as snorm8x4, apex).
NPH_BUFFER_REF(VisMeshletBoundsRef) { uvec4 b[]; };

// ---------------------------------------------------------------------------
// Transforms: the translation's high and low floats minus the camera's,
// differenced separately so the result keeps the low bits at any distance
// from the origin (Cozzi & Ring 2011, "GPU RTE").
// ---------------------------------------------------------------------------

vec3 vis_relative_translation(NphVisTransform t, vec3 camera_high, vec3 camera_low)
{
    return (vec3(t.row0.w, t.row1.w, t.row2.w) - camera_high) + (t.translation_low.xyz - camera_low);
}

vec3 vis_transform_vector(NphVisTransform t, vec3 v)
{
    return vec3(dot(t.row0.xyz, v), dot(t.row1.xyz, v), dot(t.row2.xyz, v));
}

// The largest factor by which the linear part stretches a length: the
// longest column, which bounds a sphere's radius under the transform.
float vis_max_scale(NphVisTransform t)
{
    const vec3 c0 = vec3(t.row0.x, t.row1.x, t.row2.x);
    const vec3 c1 = vec3(t.row0.y, t.row1.y, t.row2.y);
    const vec3 c2 = vec3(t.row0.z, t.row1.z, t.row2.z);
    return sqrt(max(max(dot(c0, c0), dot(c1, c1)), dot(c2, c2)));
}

// The inverse transpose of the linear part (the normal matrix), exactly as
// the reference renderer computes it per draw. The inverse's columns are
// the cross products of pairs of rows divided by the determinant, so the
// inverse transpose has them as rows.
mat3 vis_normal_matrix(NphVisTransform t)
{
    const vec3 r0 = t.row0.xyz;
    const vec3 r1 = t.row1.xyz;
    const vec3 r2 = t.row2.xyz;
    const vec3 c0 = cross(r1, r2);
    const vec3 c1 = cross(r2, r0);
    const vec3 c2 = cross(r0, r1);
    const float det = dot(r0, c0);
    return transpose(mat3(c0, c1, c2)) * (1.0 / det);
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

vec3 vis_position(NphVisMesh mesh, uint vertex)
{
    const uvec2 q = PositionsRef(mesh.positions).p[vertex];
    const vec3 unorm = vec3(q.x & 0xFFFFu, q.x >> 16u, q.y & 0xFFFFu) / 65535.0;
    return mesh.position_offset.xyz + unorm * mesh.position_scale.xyz;
}

// One byte of the meshlet triangle array (MLTR, read as u32 words).
uint vis_triangle_byte(NphVisMesh mesh, uint byte_index)
{
    const uint word = VisUintRef(mesh.meshlet_triangles).v[byte_index >> 2u];
    return (word >> ((byte_index & 3u) * 8u)) & 0xFFu;
}

// The three meshlet-local corners of triangle `t` of a meshlet whose
// triangles start at `triangle_offset`.
uvec3 vis_triangle_corners(NphVisMesh mesh, uint triangle_offset, uint t)
{
    const uint b = (triangle_offset + t) * 3u;
    return uvec3(vis_triangle_byte(mesh, b), vis_triangle_byte(mesh, b + 1u),
                 vis_triangle_byte(mesh, b + 2u));
}

// ---------------------------------------------------------------------------
// Culling (C twins in render/reference.h)
// ---------------------------------------------------------------------------

// A sphere against the frustum's five inward planes (the far plane is at
// infinity): false when it lies wholly outside one of them.
bool vis_sphere_in_frustum(NphVisViewData v, vec3 c, float r)
{
    for (int i = 0; i < 5; ++i) {
        if (dot(v.frustum[i].xyz, c) + v.frustum[i].w < -r) {
            return false;
        }
    }
    return true;
}

// The meshlet normal-cone backface test (docs/cook.md "The meshlet normal
// cone"): true when every triangle of the meshlet faces away from the
// camera at the origin. `apex` is camera relative, `axis` the stored
// snorm8 axis carried through the instance's linear part (only valid for
// similarity transforms, which keep angles).
bool vis_cone_culled(vec3 apex, vec3 axis, float cutoff)
{
    const float len = length(apex);
    return len > 0.0 && dot(apex, axis) >= cutoff * len;
}

// The screen rectangle (in [0, 1] UV, y down) and nearest depth (reverse-Z,
// infinite far plane: depth = near / distance) of a view-space sphere,
// after Mara & McGuire, "2D Polyhedral Bounds of a Clipped,
// Perspective-Projected 3D Sphere", JCGT 2013. `c` is in view space (the
// camera looks down -Z); `projection` holds P00, P11, and the near plane.
// False when the sphere reaches the near plane: nothing is known then.
bool vis_project_sphere(vec3 c, float r, vec4 projection, out vec4 rect, out float depth)
{
    const float cz = -c.z; // distance in front of the camera
    const float near = projection.z;
    rect = vec4(0.0);
    depth = 0.0;
    if (cz - r < near) {
        return false;
    }
    const float czr2 = cz * cz - r * r;
    const float vx = sqrt(c.x * c.x + czr2);
    const float minx = (vx * c.x - r * cz) / (vx * cz + r * c.x);
    const float maxx = (vx * c.x + r * cz) / (vx * cz - r * c.x);
    const float vy = sqrt(c.y * c.y + czr2);
    const float miny = (vy * c.y - r * cz) / (vy * cz + r * c.y);
    const float maxy = (vy * c.y + r * cz) / (vy * cz - r * c.y);
    // Tangents of the view angles to NDC; P11 is negative (the Y flip of
    // ADR-0008), which swaps the y bounds.
    const vec2 x = vec2(minx, maxx) * projection.x;
    const vec2 y = vec2(miny, maxy) * projection.y;
    rect = vec4(x.x, min(y.x, y.y), x.y, max(y.x, y.y)) * 0.5 + 0.5;
    depth = near / (cz - r);
    return true;
}

// Whether a rectangle (UV) at a nearest depth lies behind everything the
// hierarchical depth records there. The HZB's mip 0 is half the screen,
// each texel the farthest (smallest, reverse-Z) depth of its 2 x 2
// pixels; mip-m texel t covers mip-0 texels [t 2^m, (t + 1) 2^m), the last
// one reaching the edge (vis_hzb.comp), so clamping to the last texel stays
// conservative. The mip is chosen so the rectangle spans at most one
// texel, so a 2 x 2 footprint covers it.
bool vis_hzb_occluded(NphVisViewData v, vec4 rect, float depth)
{
    const vec4 r = clamp(rect, 0.0, 1.0);
    const vec2 size = vec2(r.z - r.x, r.w - r.y) * v.viewport.xy * 0.5; // in mip-0 texels
    const float extent = max(max(size.x, size.y), 1.0);
    const int mip_count = int(v.hzb.z);
    const int mip = min(int(ceil(log2(extent))), mip_count - 1);
    const vec2 texel_scale = v.viewport.xy * 0.5 / float(1u << uint(mip));
    const ivec2 lo = ivec2(floor(r.xy * texel_scale));
    // Vulkan's extents: each mip the one above halved, rounded down.
    const ivec2 dims = max(ivec2(v.hzb.xy) >> mip, ivec2(1));
    const ivec2 last = dims - 1;
    const ivec2 p0 = min(lo, last);
    const ivec2 p1 = min(lo + 1, last);
    const uint hzb = v.hzb_index;
    const float d = min(min(nph_fetch_2d(hzb, p0, mip).x, nph_fetch_2d(hzb, ivec2(p1.x, p0.y), mip).x),
                        min(nph_fetch_2d(hzb, ivec2(p0.x, p1.y), mip).x, nph_fetch_2d(hzb, p1, mip).x));
    return depth < d;
}

// Whether a triangle seen from the camera is front facing, from its
// clip-space vertices. Vulkan's rule ("Basic Polygon Rasterization"): with
// y down, a counter-clockwise triangle has a positive signed area, which is
// front facing for the engine's counter-clockwise pipelines (ADR-0008); a
// mirroring transform turns fronts clockwise. The sign comes from the
// determinant of the homogeneous (x, y, w) vertices, which equals the
// screen-space cross product times w0 w1 w2 and stays right when a vertex
// lies behind the camera (Olano & Greer, "Triangle Scan Conversion using
// 2D Homogeneous Coordinates", Graphics Hardware 1997).
bool vis_front_facing(vec4 c0, vec4 c1, vec4 c2, bool mirrored)
{
    const float det = determinant(mat3(c0.xyw, c1.xyw, c2.xyw));
    return (det < 0.0) != mirrored;
}

// Whether the rasteriser would produce nothing useful for a triangle with
// these clip-space vertices: a back face (when back faces are culled), or
// a triangle whose screen bounding box holds no pixel centre (the
// small-primitive test of Wihlidal, "Optimizing the Graphics Pipeline with
// Compute", GDC 2016). Triangles reaching behind the camera are left to
// the rasteriser's clipping.
bool vis_cull_triangle(vec4 c0, vec4 c1, vec4 c2, bool cull_back, bool mirrored, vec4 viewport)
{
    if (c0.w <= 0.0 || c1.w <= 0.0 || c2.w <= 0.0) {
        return false;
    }
    if (cull_back && !vis_front_facing(c0, c1, c2, mirrored)) {
        return true;
    }
    const vec2 n0 = c0.xy / c0.w;
    const vec2 n1 = c1.xy / c1.w;
    const vec2 n2 = c2.xy / c2.w;
    // Pixel centres sit at k + 0.5; a box [lo, hi] holds one when
    // floor(hi - 0.5) >= ceil(lo - 0.5). The box grows by 1/256 pixel
    // (the rasteriser's sub-pixel precision) to stay conservative.
    const vec2 s0 = (n0 * 0.5 + 0.5) * viewport.xy;
    const vec2 s1 = (n1 * 0.5 + 0.5) * viewport.xy;
    const vec2 s2 = (n2 * 0.5 + 0.5) * viewport.xy;
    const vec2 lo = min(min(s0, s1), s2) - 1.0 / 256.0;
    const vec2 hi = max(max(s0, s1), s2) + 1.0 / 256.0;
    return any(lessThan(floor(hi - 0.5), ceil(lo - 0.5)));
}

// ---------------------------------------------------------------------------
// Appending to GPU lists
// ---------------------------------------------------------------------------

// Reserves `n` entries of a list whose length is `counter` for each active
// invocation, with one atomic per subgroup: the subgroup's total is added
// once and each invocation takes its exclusive prefix (requires
// GL_KHR_shader_subgroup_arithmetic and _ballot, which the production path
// requires of the device). Every active invocation must reach it; those
// that append nothing pass 0. `base` receives the first index reserved.
#define VIS_RESERVE(counter, n, base)                                                              \
    do {                                                                                           \
        const uint vis_n_ = (n);                                                                   \
        const uint vis_total_ = subgroupAdd(vis_n_);                                               \
        const uint vis_prefix_ = subgroupExclusiveAdd(vis_n_);                                     \
        uint vis_first_ = 0u;                                                                      \
        if (vis_total_ != 0u && subgroupElect()) {                                                 \
            vis_first_ = atomicAdd(counter, vis_total_);                                           \
        }                                                                                          \
        (base) = subgroupBroadcastFirst(vis_first_) + vis_prefix_;                                 \
    } while (false)

// Adds every active invocation's `n` to a statistic.
#define VIS_ADD(counter, n)                                                                        \
    do {                                                                                           \
        const uint vis_sum_ = subgroupAdd(n);                                                      \
        if (vis_sum_ != 0u && subgroupElect()) {                                                   \
            atomicAdd(counter, vis_sum_);                                                          \
        }                                                                                          \
    } while (false)

// Counts the active invocations for which `cond` holds into a statistic.
#define VIS_COUNT(counter, cond)                                                                   \
    do {                                                                                           \
        const uint vis_c_ = subgroupBallotBitCount(subgroupBallot(cond));                          \
        if (vis_c_ != 0u && subgroupElect()) {                                                     \
            atomicAdd(counter, vis_c_);                                                            \
        }                                                                                          \
    } while (false)

// ---------------------------------------------------------------------------
// The visibility buffer
// ---------------------------------------------------------------------------

uint64_t vis_pack(float depth, uint cluster, uint triangle)
{
    return (uint64_t(floatBitsToUint(depth)) << 32ul) |
           uint64_t((cluster << NPH_VIS_TRIANGLE_BITS) | triangle);
}

float vis_depth(uint64_t v)
{
    return uintBitsToFloat(uint(v >> 32ul));
}

uint vis_cluster(uint64_t v)
{
    return uint(v & 0xFFFFFFFFul) >> NPH_VIS_TRIANGLE_BITS;
}

uint vis_triangle(uint64_t v)
{
    return uint(v) & NPH_VIS_TRIANGLE_MASK;
}

#endif

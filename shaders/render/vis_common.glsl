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
NPH_BUFFER_REF(VisCellsRef) { NphVisCell c[]; };
NPH_BUFFER_REF(VisCountersRef) { NphVisCounters c; };
NPH_BUFFER_REF(VisArgsRef) { NphVisArgs a; };
NPH_BUFFER_REF(VisClustersRef) { NphVisCluster c[]; };
// One record each: VisClusterRef(clusters)[i] is the reference to clusters[i]
// (GL_EXT_buffer_reference2), whose fields then load at fixed offsets from
// one address, where indexing the arrays recomputes the address for each
// (render_common.glsl LightRef).
NPH_BUFFER_REF(VisClusterRef) { NphVisCluster c; };
NPH_BUFFER_REF(VisInstanceRef) { NphVisInstance i; };
NPH_BUFFER_REF(VisMeshRef) { NphVisMesh m; };
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

// Perspective-correct barycentrics of a pixel at NDC `q` and their change
// one pixel to the right (`ddx`) and one down (`ddy`); `step` is the size
// of a pixel in NDC (2 / width, 2 / height; Vulkan's NDC y grows down like
// pixel rows). The screen-space barycentrics b_i are affine in q, so b_i /
// w_i and their sum are too; their ratio is the perspective-correct
// weight, exact for neighbours as well.
struct VisBarycentrics {
    vec3 b;
    vec3 ddx;
    vec3 ddy;
};

VisBarycentrics vis_barycentrics(vec4 c0, vec4 c1, vec4 c2, vec2 q, vec2 step)
{
    const vec3 inv_w = 1.0 / vec3(c0.w, c1.w, c2.w);
    const vec2 n0 = c0.xy * inv_w.x;
    const vec2 e1 = c1.xy * inv_w.y - n0;
    const vec2 e2 = c2.xy * inv_w.z - n0;
    const float inv_area = 1.0 / (e1.x * e2.y - e1.y * e2.x);
    const vec2 d = q - n0;
    const float b1 = (d.x * e2.y - d.y * e2.x) * inv_area;
    const float b2 = (e1.x * d.y - e1.y * d.x) * inv_area;
    // Gradients of b1 and b2 in NDC; b0's is minus their sum.
    const vec2 g1 = vec2(e2.y, -e2.x) * inv_area;
    const vec2 g2 = vec2(-e1.y, e1.x) * inv_area;
    const vec2 g0 = -(g1 + g2);
    const vec3 f = vec3(1.0 - b1 - b2, b1, b2) * inv_w;
    const vec3 fx = vec3(g0.x, g1.x, g2.x) * inv_w * step.x;
    const vec3 fy = vec3(g0.y, g1.y, g2.y) * inv_w * step.y;
    const float s = f.x + f.y + f.z;
    VisBarycentrics r;
    r.b = f / s;
    r.ddx = (f + fx) / (s + fx.x + fx.y + fx.z) - r.b;
    r.ddy = (f + fy) / (s + fy.x + fy.y + fy.z) - r.b;
    return r;
}

// The material table index of a material handle value from the material
// map: its slot when the table's record there still carries that handle,
// else the default material (a destroyed or unset material).
uint vis_resolve_material(VisViewRef view, uint handle)
{
    if (handle == 0u) {
        return view.v.default_material;
    }
    const uint index = handle & view.v.material_index_mask;
    if (index >= view.v.default_material ||
        MaterialsRef(view.v.materials).m[index].handle != handle) {
        return view.v.default_material;
    }
    return index;
}

// The material table index of submesh slot `slot` of an instance (its
// material map's entry; the default material past its slots).
uint vis_instance_material(VisViewRef view, NphVisInstance inst, uint slot)
{
    const uint handle = slot < inst.material_count
                            ? VisUintRef(view.v.material_map).v[inst.material_base + slot]
                            : 0u;
    return vis_resolve_material(view, handle);
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
// infinity): false when it lies wholly outside one of them. The view is
// read through its reference: a copy of the struct, which holds arrays, is
// what Ubuntu 24.04's glslang (14.0) may give the buffer's ArrayStride
// decorations, which spirv-val rejects (docs/build.md "Shaders").
bool vis_sphere_in_frustum(VisViewRef view, vec3 c, float r)
{
    for (int i = 0; i < 5; ++i) {
        const vec4 plane = view.v.frustum[i];
        if (dot(plane.xyz, c) + plane.w < -r) {
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

// One axis of vis_project_sphere_clipped: the lower and upper bounds, as
// (axis coordinate, z) points in the plane of the axis and the view
// direction, of a sphere whose centre in that plane is `c` (Mara &
// McGuire 2013, listing 1). Each bound is where a ray from the camera
// grazes the sphere, unless that point lies behind the near plane `near_z`
// (negative) or the camera is inside the sphere: then the bound is where
// the near plane cuts the sphere.
void vis_sphere_axis_bounds(vec2 c, float r, float near_z, out vec2 lower, out vec2 upper)
{
    const float t2 = dot(c, c) - r * r;
    const bool inside = t2 <= 0.0;
    float cos_t = 0.0;
    float sin_t = 0.0;
    if (!inside) {
        const float len = length(c);
        cos_t = sqrt(t2) / len;
        sin_t = r / len;
    }
    const bool clipped = c.y + r >= near_z;
    float k = sqrt(max(r * r - (near_z - c.y) * (near_z - c.y), 0.0));
    vec2 b[2];
    for (int i = 0; i < 2; ++i) {
        b[i] = vec2(0.0, near_z);
        if (!inside) {
            // c rotated by the tangent's angle, scaled to the tangent point.
            b[i] = vec2(cos_t * c.x - sin_t * c.y, sin_t * c.x + cos_t * c.y) * cos_t;
        }
        if (clipped && (inside || b[i].y > near_z)) {
            b[i] = vec2(c.x + k, near_z);
        }
        sin_t = -sin_t;
        k = -k;
    }
    upper = b[0];
    lower = b[1];
}

// The screen rectangle (UV, y down, not clamped) of the part of a
// view-space sphere in front of the near plane, which the caller has
// found to be in the frustum (Mara & McGuire, "2D Polyhedral Bounds of a
// Clipped, Perspective-Projected 3D Sphere", JCGT 2013): unlike
// vis_project_sphere it bounds spheres that reach the near plane or hold
// the camera, as lights near the camera do.
vec4 vis_project_sphere_clipped(vec3 c, float r, vec4 projection)
{
    const float near_z = -projection.z;
    vec2 lx;
    vec2 ux;
    vec2 ly;
    vec2 uy;
    vis_sphere_axis_bounds(vec2(c.x, c.z), r, near_z, lx, ux);
    vis_sphere_axis_bounds(vec2(c.y, c.z), r, near_z, ly, uy);
    // The bounds' points lie in front of the near plane: w = -z > 0.
    const vec2 x = vec2(lx.x / -lx.y, ux.x / -ux.y) * projection.x;
    const vec2 y = vec2(ly.x / -ly.y, uy.x / -uy.y) * projection.y;
    return vec4(min(x.x, x.y), min(y.x, y.y), max(x.x, x.y), max(y.x, y.y)) * 0.5 + 0.5;
}

// The sphere around everything a light can reach (camera relative; w the
// radius): a point light's range, or for a spot light the smallest sphere
// around its cone of the outer angle capped at the range. For half-angles
// up to 45 degrees that sphere passes through the apex and the rim, its
// centre on the axis range / (2 cos a) from the apex; for wider cones it
// is the rim's own circle's sphere, centred range cos a along the axis
// (Wronski, "Cull that Cone!", 2017). The cap's points lie inside both.
vec4 vis_light_sphere(vec3 position, vec3 direction, float range, uint type, float cos_outer,
                      float sin_outer)
{
    if (type == NPH_RENDER_LIGHT_SPOT) {
        if (cos_outer >= 0.70710678) {
            const float t = range / (2.0 * cos_outer);
            return vec4(position + direction * t, t);
        }
        return vec4(position + direction * (range * cos_outer), range * sin_outer);
    }
    return vec4(position, range);
}

// The light grid's bounds are grown by this fraction so that a pixel inside
// a light's reach is never binned or tiled outside it by rounding: the
// distances, rectangles and positions of the pixels and of the lights are
// computed differently.
#define VIS_LIGHT_SLACK 1e-4

// The box around a light tile's part of the view between the view
// distances dist.x and dist.y (0 < x <= y), view space (`lo`, `hi`).
// `slopes` bound the tile's rays: x / -z in .x (lower) and .z (upper), y / -z
// in .y and .w. The box is grown by VIS_LIGHT_SLACK of its largest
// coordinate, so that a surface point at a distance the tile's depths bound
// lies inside it however its position was rounded.
void vis_tile_box(vec4 slopes, vec2 dist, out vec3 lo, out vec3 hi)
{
    const vec4 a = abs(slopes);
    const float grow = VIS_LIGHT_SLACK * dist.y * (1.0 + max(max(a.x, a.y), max(a.z, a.w)));
    lo = vec3(min(slopes.xy * dist.x, slopes.xy * dist.y), -dist.y) - grow;
    hi = vec3(max(slopes.zw * dist.x, slopes.zw * dist.y), -dist.x) + grow;
}

// Whether a view-space sphere (w the radius) meets the box [lo, hi].
bool vis_box_meets_sphere(vec3 lo, vec3 hi, vec4 sphere)
{
    const vec3 d = max(max(lo - sphere.xyz, sphere.xyz - hi), vec3(0.0));
    return dot(d, d) <= sphere.w * sphere.w;
}

// Whether a view-space sphere meets the box around a light tile's part of
// the view between the view distances dist.x and dist.y (vis_tile_box).
bool vis_tile_meets_sphere(vec4 slopes, vec2 dist, vec4 sphere)
{
    vec3 lo;
    vec3 hi;
    vis_tile_box(slopes, dist, lo, hi);
    return vis_box_meets_sphere(lo, hi, sphere);
}

// Whether a rectangle (UV) at a nearest depth lies behind everything the
// hierarchical depth records there. The HZB's mip 0 is half the screen,
// each texel the farthest (smallest, reverse-Z) depth of its 2 x 2
// pixels; mip-m texel t covers mip-0 texels [t 2^m, (t + 1) 2^m), the last
// one reaching the edge (vis_hzb.comp), so clamping to the last texel stays
// conservative. The mip is chosen so the rectangle spans at most one
// texel, so a 2 x 2 footprint covers it.
bool vis_hzb_occluded(VisViewRef view, vec4 rect, float depth)
{
    const vec2 viewport = view.v.viewport.xy;
    const vec4 hzb_size = view.v.hzb;
    const vec4 r = clamp(rect, 0.0, 1.0);
    const vec2 size = vec2(r.z - r.x, r.w - r.y) * viewport * 0.5; // in mip-0 texels
    const float extent = max(max(size.x, size.y), 1.0);
    const int mip_count = int(hzb_size.z);
    const int mip = min(int(ceil(log2(extent))), mip_count - 1);
    const vec2 texel_scale = viewport * 0.5 / float(1u << uint(mip));
    const ivec2 lo = ivec2(floor(r.xy * texel_scale));
    // Vulkan's extents: each mip the one above halved, rounded down.
    const ivec2 dims = max(ivec2(hzb_size.xy) >> mip, ivec2(1));
    const ivec2 last = dims - 1;
    const ivec2 p0 = min(lo, last);
    const ivec2 p1 = min(lo + 1, last);
    const uint hzb = view.v.hzb_index;
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
// Compute", GDC 2016). The facing holds whatever the w's (the homogeneous
// determinant), so back faces go even when they reach behind the camera:
// the opaque raster's fragment stage then never discards, which its early
// depth test requires. The box test needs every vertex in front, and
// leaves the rest to the rasteriser's clipping.
bool vis_cull_triangle(vec4 c0, vec4 c1, vec4 c2, bool cull_back, bool mirrored, vec4 viewport)
{
    if (cull_back && !vis_front_facing(c0, c1, c2, mirrored)) {
        return true;
    }
    if (c0.w <= 0.0 || c1.w <= 0.0 || c2.w <= 0.0) {
        return false;
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

// ---------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------

// Where the surface seen at pixel `q` (visibility-buffer texel `v`, 0 for
// the sky) lay in the previous frame, in UV without jitter; false when it
// lay behind the previous camera. The point, in this frame's clip space as
// the sample at q was drawn (with the jitter), goes to the previous frame's
// without its jitter: through the previous transform for an instance that
// moved (NPH_VIS_INSTANCE_MOVED, which its clusters carry), and otherwise
// with the camera's move alone (NphVisViewData.reproject), which leaves the
// sky's points at infinity to the camera's rotation. The temporal resolve
// (taa.frag) and the motion blur's motion (motion.comp) share it.
// ---------------------------------------------------------------------------
// Levels of detail (render/cluster_lod.h; docs/render.md "Levels of detail")
// ---------------------------------------------------------------------------

// A hierarchy node and a group, as render/cluster_lod.h has them. Read
// field by field: Ubuntu 24.04's glslang (14.0) gives a local copy of a
// struct with array members the buffer's array types (docs/build.md
// "Shaders").
struct NphVisLodNode {
    vec4 sphere[NPH_VIS_LOD_CHILDREN];       // xyz centre, w radius (mesh space)
    float error[NPH_VIS_LOD_CHILDREN];       // the largest group error below
    float maker_error[NPH_VIS_LOD_CHILDREN]; // the smallest maker error below
    uint child[NPH_VIS_LOD_CHILDREN];        // node, group | NPH_VIS_LOD_LEAF, or NPH_VIS_LOD_NONE
};
struct NphVisLodGroup {
    vec3 center;
    float radius;
    float error;
    uint level;
    uint cluster_offset;
    uint cluster_count;
};
NPH_BUFFER_REF(VisLodNodesRef) { NphVisLodNode n[]; };
NPH_BUFFER_REF(VisLodGroupsRef) { NphVisLodGroup g[]; };

// The root group's error (render/cluster_lod.h: the largest float): always
// too coarse, whatever the view and the instance's scale.
#define VIS_LOD_ROOT_ERROR 3.402823466e38

// Whether a group of a mesh's levels of detail is too coarse for the view
// (C twin: nph_render_lod_projected_error over the threshold): its error at
// the instance's scale, times the pixels a unit spans at a distance of one,
// over the distance to its sphere's nearest point (at least the near
// plane), is more than the threshold. With k = view.lod.x (pixels a unit
// spans at distance one, over the threshold), the instance's scale s (the
// longest column of its linear part), and the camera-relative centre c,
//
//     s e k > max(|c| - s r, near)  <=>  s^2 (e k)^2 > near^2
//                                        and s^2 (e k + r)^2 > |c|^2,
//
// which needs no square root. The hierarchy's steps (vis_cull_nodes.comp)
// take a group's clusters where it is too coarse, and the meshlet cull
// (vis_cull_meshlets.comp) drops a cluster where the group that made it is:
// the two must decide alike for every group, bit for bit, or a part of the
// mesh is drawn at two levels, or at none. So the decision is made here
// alone, from the instance's record and the group's own numbers, with every
// step `precise` (SPIR-V NoContraction: no fused or reordered arithmetic),
// where Vulkan rounds each addition and multiplication correctly.
bool vis_lod_group_too_coarse(VisViewRef view, NphVisTransform tr, vec4 sphere, float error)
{
    if (error >= VIS_LOD_ROOT_ERROR) {
        return true;
    }
    precise float tx = (tr.row0.w - view.v.camera_high.x) + (tr.translation_low.x - view.v.camera_low.x);
    precise float ty = (tr.row1.w - view.v.camera_high.y) + (tr.translation_low.y - view.v.camera_low.y);
    precise float tz = (tr.row2.w - view.v.camera_high.z) + (tr.translation_low.z - view.v.camera_low.z);
    precise float cx = ((tr.row0.x * sphere.x + tr.row0.y * sphere.y) + tr.row0.z * sphere.z) + tx;
    precise float cy = ((tr.row1.x * sphere.x + tr.row1.y * sphere.y) + tr.row1.z * sphere.z) + ty;
    precise float cz = ((tr.row2.x * sphere.x + tr.row2.y * sphere.y) + tr.row2.z * sphere.z) + tz;
    precise float d2 = (cx * cx + cy * cy) + cz * cz;
    precise float s0 = (tr.row0.x * tr.row0.x + tr.row1.x * tr.row1.x) + tr.row2.x * tr.row2.x;
    precise float s1 = (tr.row0.y * tr.row0.y + tr.row1.y * tr.row1.y) + tr.row2.y * tr.row2.y;
    precise float s2 = (tr.row0.z * tr.row0.z + tr.row1.z * tr.row1.z) + tr.row2.z * tr.row2.z;
    precise float s = max(max(s0, s1), s2);
    precise float ek = error * view.v.lod.x;
    precise float reach = ek + sphere.w;
    precise float near2 = view.v.lod.y * view.v.lod.y;
    precise float far = s * (ek * ek);
    precise float wide = s * (reach * reach);
    return far > near2 && wide > d2;
}

// The steps' prune of a child that is a node: whether a group below it may
// be too coarse (vis_lod_group_too_coarse). The node's sphere holds the
// groups' and its error is their largest, so their test on the node's
// numbers bounds theirs; it is loosened here past the rounding between
// them (a part in 2^12 of the error and the radius, and 2^-16 of the
// magnitudes the centre's rounding is relative to), so that a prune never
// drops a group the exact test takes. `t` and `scale` are the instance's
// (vis_relative_translation, vis_max_scale).
bool vis_lod_node_too_coarse(VisViewRef view, NphVisTransform tr, vec3 t, float scale,
                             vec4 sphere, float error)
{
    if (error >= VIS_LOD_ROOT_ERROR) {
        return true;
    }
    const vec3 m = vis_transform_vector(tr, sphere.xyz);
    const vec3 c = m + t;
    const vec3 magnitude = abs(m) + abs(t);
    const float loose = 1.0 + 1.0 / 4096.0;
    const float e = error * view.v.lod.x * scale * loose;
    const float reach = e + sphere.w * scale * loose +
                        (magnitude.x + magnitude.y + magnitude.z) * (1.0 / 65536.0);
    return e > view.v.lod.y && reach * reach > dot(c, c);
}

// Whether every group whose sphere lies inside `sphere` (mesh space) and
// whose error is at least `error` is surely too coarse for the view
// (vis_lod_group_too_coarse): the distance to the sphere's farthest point
// bounds each group's nearest, tightened past the rounding between the
// tests as vis_lod_node_too_coarse is loosened, so that it never says so of
// a group the exact test finds fine. An error of 0 never is. Two uses: the
// walk skips a child whose makers below (their smallest error, `maker_error`
// of the node) are all too coarse, since the meshlet cull would drop every
// cluster it leads to; and the instance cull draws level 0 directly where
// all of level 0's consumers are (NphVisMesh.lod_level0_error).
bool vis_lod_surely_too_coarse(VisViewRef view, NphVisTransform tr, vec3 t, float scale,
                               vec4 sphere, float error)
{
    const vec3 m = vis_transform_vector(tr, sphere.xyz);
    const vec3 c = m + t;
    const vec3 magnitude = abs(m) + abs(t);
    const float e = error * view.v.lod.x * scale * (1.0 - 1.0 / 4096.0);
    const float far = length(c) + sphere.w * scale * (1.0 + 1.0 / 4096.0) +
                      (magnitude.x + magnitude.y + magnitude.z) * (1.0 / 65536.0);
    return e > far && e > view.v.lod.y * (1.0 + 1.0 / 4096.0);
}

// ---------------------------------------------------------------------------
// Cells of a world partition (render/renderer.h nph_render_cell_create;
// ADR-0062)
// ---------------------------------------------------------------------------

// Whether cell `cell` (its index plus one, from an instance's flags) is drawn
// as its proxy in the view: with the levels of detail, where its box's
// nearest point is at least the proxy's distance away (what no camera that
// far sees is not in the proxy), and the proxy's finest error is fine from
// there (vis_lod_group_too_coarse's test at the box's nearest point, scale
// 1: e k <= max(d, near)). A cell's instances and its proxy decide from the
// same record and view, by the same arithmetic, so the proxy is drawn exactly
// where they are not.
bool vis_cell_proxied(VisViewRef view, uint cell)
{
    if ((view.v.flags & NPH_VIS_VIEW_LOD) == 0u) {
        return false;
    }
    const NphVisCell c = VisCellsRef(view.v.cells).c[cell - 1u];
    const vec3 centre =
        (c.center_high.xyz - view.v.camera_high.xyz) + (c.center_low.xyz - view.v.camera_low.xyz);
    const float d = length(max(abs(centre) - c.extent.xyz, vec3(0.0)));
    return d >= c.center_high.w && c.center_low.w * view.v.lod.x <= max(d, view.v.lod.y);
}

bool vis_previous_uv(VisViewRef view, ivec2 q, uint64_t v, out vec2 uv)
{
    const vec2 ndc = (vec2(q) + 0.5) * view.v.viewport.zw * 2.0 - 1.0;
    const vec4 point = vec4(ndc, v == 0ul ? 0.0 : vis_depth(v), 1.0);
    vec4 clip;
    bool moved = false;
    if (v != 0ul) {
        // The cluster's flags say whether its instance moved, so a still
        // one's record is not read.
        VisClustersRef clusters = VisClustersRef(view.v.clusters);
        const uint c = vis_cluster(v);
        if ((clusters.c[c].flags & NPH_VIS_CLUSTER_MOVED) != 0u) {
            moved = true;
            // Homogeneous and camera relative.
            ViewRef rview = ViewRef(view.v.render_view);
            const vec4 x = rview.v.inv_view_proj * point;
            const uint slot = clusters.c[c].instance;
            const NphVisTransform transform = VisInstancesRef(view.v.instances).i[slot].transform;
            const vec3 t = vis_relative_translation(transform, view.v.camera_high.xyz,
                                                    view.v.camera_low.xyz);
            // The point in the instance's own space (the inverse of the
            // linear part is the normal matrix transposed), out again
            // through the previous transform, relative to the previous camera.
            const vec3 local = transpose(vis_normal_matrix(transform)) * (x.xyz / x.w - t);
            const NphVisTransform pt = VisTransformsRef(view.v.prev_transforms).t[slot];
            const vec3 then = vis_transform_vector(pt, local) +
                              vis_relative_translation(pt, view.v.prev_camera_high.xyz,
                                                       view.v.prev_camera_low.xyz);
            clip = view.v.prev_view_proj * vec4(then, 1.0);
        }
    }
    if (!moved) {
        clip = view.v.reproject * point;
    }
    uv = clip.xy / clip.w * 0.5 + 0.5;
    return clip.w > 0.0;
}

#endif

// SPDX-License-Identifier: Apache-2.0
// The visibility-buffer resolve (docs/render.md "Shading"): for each pixel
// it decodes the cluster and triangle the raster left, fetches the
// triangle's three vertices, rebuilds the attributes at the pixel from
// perspective-correct barycentrics, and shades with surface.glsl, the
// reference renderer's own shading (Burns & Hunt, "The Visibility Buffer:
// A Cache-Friendly Approach to Deferred Shading", JCGT 2013). Texture
// filtering needs screen-space derivatives that a compute shader does not
// have, so the barycentrics of the pixel's right and lower neighbours are
// computed too, as the rasteriser's quad differences would be (Schied &
// Dachsbacher, "Deferred Attribute Interpolation for Memory-Efficient
// Deferred Shading", HPG 2015). Where nothing was drawn it writes the
// background, as the reference's sky pass does.
//
// Writes pre-exposed radiance into the HDR image. Its push constants
// (NphVisResolvePush) carry the images' indices and the view's values every
// invocation reads (the transform, the target's size, the tables'
// addresses): the shader takes them as constant-bank operands, where values
// loaded from the view record take registers, which the resolve runs short
// of (docs/render.md "Shading").
//
// Compiled twice: vis_resolve.comp, and vis_resolve_rt.comp with ray
// queries (NPH_RAY_QUERY), which traces shadow rays for the lights that
// cast them (rt_common.glsl); the renderer takes the second when a light
// casts shadows. Each is specialised on whether the frame has the sun's
// virtual shadow map (k_vsm) and the radiance cache (k_gi), and on whether
// it draws the debug views (k_debug): the others compile without their
// branches.
//
// A pixel of a voxel cluster (render/voxel.h; ADR-0061) is the voxel
// resolve's (vis_voxel_resolve.glsl): the resolve writes nothing there and
// lists the pixel's tile for it, once a subgroup that saw one. It tests for
// them only where specialised to (k_voxels: the renderer holds a mesh with
// voxel levels, or the frame draws a debug view).
#ifndef NPH_VIS_RESOLVE_GLSL
#define NPH_VIS_RESOLVE_GLSL

#extension GL_KHR_shader_subgroup_basic : require
#include "vis_common.glsl"
#include "nph_bindless_int64.glsl"

#ifdef NPH_RAY_QUERY
#include "rt_common.glsl"
// The view's top-level structure and the camera's place in it, set first
// thing in main(): the shadow test runs deep in the light loop.
uint64_t g_tlas;
vec3 g_tlas_offset;
#define NPH_SURFACE_SHADOW(p, l, distance)                                                         \
    nph_shadow_ray(g_tlas, (p).position + g_tlas_offset, (p).face_normal, (l), (distance))
#endif

// The sun's virtual shadow map where the view has one (vsm_common.glsl):
// the pipelines are specialised on it, so frames without one keep the
// registers its test would hold.
#define NPH_VSM_SAMPLING
#include "vsm_common.glsl"
layout(constant_id = 1) const bool k_vsm = false;
VsmViewRef g_vsm; // set first thing in main()
#define NPH_SURFACE_VSM_ACTIVE k_vsm
#define NPH_SURFACE_VSM(p)     vsm_shadow(g_vsm, (p).position, (p).face_normal, (p).n)

// The ambient occlusion where the frame has it (ao.c), at the pixel the
// invocation shades, of reverse-Z depth g_depth; set first thing in main().
ivec2 g_pixel;
float g_depth;
uint g_ao; // the occlusion, one a pixel (ao_denoise.comp); NPH_VIS_NO_AO: none

// The occlusion at the pixel.
float vis_ambient_occlusion()
{
    return texelFetch(nph_textures_2d[g_ao], g_pixel, 0).r;
}
#define NPH_SURFACE_AO (g_ao != NPH_VIS_NO_AO ? vis_ambient_occlusion() : 1.0)

// The radiance cache where the frame has it (gi.c): the pipelines are
// specialised on it, so frames without it keep the registers its lookup
// would hold. main() looks the pixel's cell up as soon as it has the
// pixel's point, before the triangle's attributes, and the shading blends
// the cell's irradiance in (surface.glsl): the resolve runs at its register
// limit, and of the places measured (here, the shading's indirect light,
// after the attributes) this one costs least (docs/adr/0039).
#include "gi_common.glsl"
layout(constant_id = 2) const bool k_gi = false;
vec4 g_gi_cell; // set in main() where k_gi
#define NPH_SURFACE_GI_ACTIVE k_gi
#define NPH_SURFACE_GI        g_gi_cell

#define NPH_SURFACE_SAMPLE(tex, smp, s)                                                            \
    textureGrad(sampler2D(nph_textures_2d[nonuniformEXT(tex)], nph_samplers[nonuniformEXT(smp)]), \
                (s).uv, (s).uv_dx, (s).uv_dy)
#define NPH_SURFACE_GRID_LIGHTS
layout(constant_id = 3) const bool k_debug = false;
#define NPH_SURFACE_DEBUG_MODE(view) (k_debug ? (view).v.debug_mode : NPH_RENDER_DEBUG_NONE)
// Whether voxel pixels may appear (RenderVis.resolve_voxels): the test for
// them, though never taken, costs the triangles' path 0.5 to 1.7 % of the
// resolve's time where it is compiled in (measured on render_perf's 4K
// field and LOD scenes), wherever it stands.
layout(constant_id = 4) const bool k_voxels = false;
#include "surface.glsl"

layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant, scalar) uniform Push { NphVisResolvePush pc; };

// Whether the HDR target is B10G11R11, whose coarse steps are dithered.
layout(constant_id = 0) const bool k_dither = false;

// With `dither` the colour is rounded to the HDR target's grid at random
// (debug views are stored as they are, for reading).
void store(ivec2 p, vec3 color, bool dither)
{
    if (dither) {
        color = nph_dither_b10g11r11(color, nph_gradient_noise(vec2(p)));
    }
    imageStore(nph_images_2d[nonuniformEXT(pc.hdr_index)], p, vec4(color, 1.0));
}

// A bright colour from a key (an integer hash: Wang, "Integer Hash
// Function", 1997).
vec3 vis_debug_color(uint key)
{
    uint h = (key ^ 61u) ^ (key >> 16u);
    h *= 9u;
    h ^= h >> 4u;
    h *= 0x27d4eb2du;
    h ^= h >> 15u;
    return vec3(0.2) + 0.8 * vec3(h & 0xFFu, (h >> 8u) & 0xFFu, (h >> 16u) & 0xFFu) / 255.0;
}

vec3 background(ViewRef rview, vec2 q)
{
    vec3 color;
    if (uint(rview.v.background.w) == NPH_RENDER_BACKGROUND_COLOR) {
        color = rview.v.background.rgb;
    } else {
        // A point on the near plane (depth 1 under reverse-Z) gives the
        // ray; the camera sits at the origin.
        const vec4 p = rview.v.inv_view_proj * vec4(q, 1.0, 1.0);
        const vec3 dir = normalize(p.xyz / p.w);
        color = textureLod(samplerCube(nph_textures_cube[nonuniformEXT(rview.v.env_index)],
                                       nph_samplers[nonuniformEXT(rview.v.clamp_sampler)]),
                           dir, 0.0).rgb *
                rview.v.ibl_intensity;
    }
    return min(color * rview.v.exposure, vec3(NPH_RENDER_HALF_MAX));
}

// Lists the tile of pixel `p`, a voxel cluster's, for the voxel resolve: the
// first invocation of the subgroup sets the tile's bit, and the one that
// set it appends the tile and raises the voxel resolve's workgroups (a
// subgroup lies in one workgroup, so in one tile).
void list_voxel_tile(VisViewRef view, ivec2 p)
{
    if (subgroupElect()) {
        const uvec2 tile = uvec2(p) / uint(NPH_VIS_RESOLVE_TILE);
        const uint index = tile.y * view.v.resolve_tiles_x + tile.x;
        const uint bit = 1u << (index & 31u);
        if ((atomicOr(VisUintRef(view.v.voxel_tile_bits).v[index >> 5u], bit) & bit) == 0u) {
            const uint j = atomicAdd(VisCountersRef(view.v.counters).c.voxel_tiles, 1u);
            VisUintRef(view.v.voxel_tiles).v[j] = tile.x | (tile.y << 16u);
            atomicMax(VisArgsRef(view.v.args).a.voxel_resolve_args[0], j + 1u);
        }
    }
}

void main()
{
    VisViewRef view = VisViewRef(pc.view);
    const ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    const vec2 size = vec2(float(pc.size & 0xFFFFu), float(pc.size >> 16u));
    if (p.x >= int(size.x) || p.y >= int(size.y)) {
        return;
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
    // Decided here, so that nothing of the view stays live to the store.
    const bool dither = k_dither && debug == NPH_RENDER_DEBUG_NONE;
    const vec2 inv_size = vec2(pc.inv_width, pc.inv_height);
    const vec2 q = (vec2(p) + 0.5) * inv_size * 2.0 - 1.0;
    const uint64_t v = imageLoad(nph_u64images_2d[nonuniformEXT(pc.vis_index)], p).x;
    // A voxel pixel leaves here, before anything of the triangles' path is
    // live: tested in the cluster branch below, the early exit also moved
    // the compiler's spills (the production variant's from 1792 to 2304
    // bytes). The cluster debug views draw voxels as triangles.
    if (k_voxels && v != 0ul && debug < NPH_RENDER_DEBUG_CLUSTERS &&
        (VisClustersRef(pc.clusters).c[vis_cluster(v)].flags & NPH_VIS_CLUSTER_VOXELS) != 0u) {
        list_voxel_tile(view, p);
        return;
    }
    g_depth = uintBitsToFloat(uint(v >> 32ul));
    vec3 color;
    if (v == 0ul) {
        color = background(rview, q);
    } else if (debug >= NPH_RENDER_DEBUG_CLUSTERS) {
        // What the culling and the raster produced, as hashed colours.
        const uint g = vis_cluster(v);
        const uint key = debug == NPH_RENDER_DEBUG_CLUSTERS    ? g
                         : debug == NPH_RENDER_DEBUG_TRIANGLES ? (g << 7u) ^ vis_triangle(v)
                                                               : VisClustersRef(pc.clusters).c[g].instance;
        color = vis_debug_color(key);
    } else {
        const NphVisCluster cluster = VisClusterRef(pc.clusters)[vis_cluster(v)].c;
        const NphVisInstance inst = VisInstanceRef(pc.instances)[cluster.instance].i;
        const NphVisMesh mesh = VisMeshRef(pc.meshes)[inst.mesh].m;
        const uvec4 m = VisMeshletsRef(mesh.meshlets).m[cluster.meshlet];
        const uvec3 corners = vis_triangle_corners(mesh, m.y, vis_triangle(v));
        VisUintRef mlvx = VisUintRef(mesh.meshlet_vertices);
        const uvec3 vtx = uvec3(mlvx.v[m.x + corners.x], mlvx.v[m.x + corners.y],
                                mlvx.v[m.x + corners.z]);

        // The vertices as the raster saw them (the same arithmetic).
        const vec3 t = vis_relative_translation(inst.transform, view.v.camera_high.xyz,
                                                view.v.camera_low.xyz);
        const vec3 w0 = vis_transform_vector(inst.transform, vis_position(mesh, vtx.x)) + t;
        const vec3 w1 = vis_transform_vector(inst.transform, vis_position(mesh, vtx.y)) + t;
        const vec3 w2 = vis_transform_vector(inst.transform, vis_position(mesh, vtx.z)) + t;
        // The transform from the push constants where the frame has the
        // radiance cache, from the view's record otherwise: each variant
        // compiles to fewer registers so on the RTX 3060 Ti (the same values;
        // docs/render.md "Shading").
        const mat4 view_proj = k_gi ? pc.view_proj : view.v.view_proj;
        const vec4 c0 = view_proj * vec4(w0, 1.0);
        const vec4 c1 = view_proj * vec4(w1, 1.0);
        const vec4 c2 = view_proj * vec4(w2, 1.0);
        const VisBarycentrics bary = vis_barycentrics(c0, c1, c2, q, 2.0 * inv_size);
        const bool mirrored = (cluster.flags & NPH_VIS_CLUSTER_MIRRORED) != 0u;
        // What needs the clip and world positions first, so that they end
        // here rather than stay live across the attributes' decode: this
        // shader runs at its register limit, and what does not fit spills
        // to shared memory (docs/render.md "Shading").
        NphSurface s;
        s.front_facing = vis_front_facing(c0, c1, c2, mirrored);
        const mat3 world = mat3(w0, w1, w2);
        s.position = world * bary.b;
        s.position_dx = world * bary.ddx;
        s.position_dy = world * bary.ddy;
        s.face_normal = normalize(cross(w1 - w0, w2 - w0));
        if (k_gi) {
            // The point and the triangle's normal as the cache's update
            // keys them (gi_update.comp: the same arithmetic).
            g_gi_cell = gi_surface_cell(GiViewRef(rview.v.gi), p, s.position, s.face_normal);
        }

        // Each attribute interpolated as soon as it is unpacked.
        AttribsRef attribs = AttribsRef(mesh.attribs);
        const uvec4 a0 = attribs.a[vtx.x];
        const uvec4 a1 = attribs.a[vtx.y];
        const uvec4 a2 = attribs.a[vtx.z];
        const mat3x2 uvs =
            mat3x2(unpackHalf2x16(a0.z), unpackHalf2x16(a1.z), unpackHalf2x16(a2.z));
        s.uv = uvs * bary.b;
        s.uv_dx = uvs * bary.ddx;
        s.uv_dy = uvs * bary.ddy;
        const mat3 normals = mat3(nph_oct_decode(unpackSnorm2x16(a0.x)),
                                  nph_oct_decode(unpackSnorm2x16(a1.x)),
                                  nph_oct_decode(unpackSnorm2x16(a2.x)));
        s.normal = vis_normal_matrix(inst.transform) * (normals * bary.b);
        const vec4 t0 = unpackSnorm4x8(a0.y);
        const vec4 t1 = unpackSnorm4x8(a1.y);
        const vec4 t2 = unpackSnorm4x8(a2.y);
        // Handedness as the reference's vertex stage sets it: the stored
        // sign, flipped by a mirroring transform.
        const float hand = mirrored ? -1.0 : 1.0;
        const vec3 handedness = vec3(t0.w < 0.0 ? -hand : hand, t1.w < 0.0 ? -hand : hand,
                                     t2.w < 0.0 ? -hand : hand);
        s.tangent = vec4(vis_transform_vector(inst.transform, mat3(t0.xyz, t1.xyz, t2.xyz) * bary.b),
                         dot(handedness, bary.b));
        s.color = mat3x4(unpackUnorm4x8(a0.w), unpackUnorm4x8(a1.w), unpackUnorm4x8(a2.w)) * bary.b;
        s.pixel = vec2(p) + 0.5;
        s.depth = vis_depth(v);

        if (debug == NPH_RENDER_DEBUG_IRRADIANCE) {
            color = nph_surface_irradiance_view(rview, s, k_gi ? g_gi_cell : vec4(0.0));
        } else {
            const NphRenderMaterialData material =
                MaterialRef(view.v.materials)[cluster.material].m;
            color = nph_surface_shade(rview, material, s, nph_surface_base_color(material, s));
        }
    }
    store(p, color, dither);
}

#endif

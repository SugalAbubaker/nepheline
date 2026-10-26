// SPDX-License-Identifier: Apache-2.0
#pragma once

// Voxels: the far field of aggregate geometry (foliage, twigs, debris) in the
// cluster levels of detail (render/cluster_lod.h; docs/render.md "Voxels";
// ADR-0061). Where the levels' edge collapses stall on many small pieces, a
// group of clusters becomes voxels instead: bricks of 4 x 4 x 4 cells of one
// size, each occupied cell a voxel that holds what the cell's geometry looks
// like from outside at the cell's scale.
//
//   - Its footprint: a shape inside a box inside the cell. A ray through the
//     cell meets the voxel exactly where it meets the footprint, which the
//     builder sizes so that its projected area along each axis is the
//     geometry's coverage of the cell along that axis (the share of rays it
//     stops), within the geometry's own bounds there: rays through a canopy
//     of footprints stop about as often as through the needles they stand
//     for (Cook et al., "Stochastic Simplification of Aggregate Detail",
//     SIGGRAPH 2007, its area preservation, without the randomness; Heitz &
//     Neyret, "Representing Appearance and Pre-filtering Subpixel Data in
//     Sparse Voxel Octrees", HPG 2012, on coverage that correlated geometry
//     inside a voxel must not count twice). The shapes:
//       - a surface's, the box: it keeps the whole extent of a sheet across
//         the cell, so a surface stays closed from cell to cell;
//       - a needle's (one straight piece across the cell), a capsule along
//         the piece's axis, whose ends are opposite corners of the box (and
//         which reaches past the box by its radius, held to the cell): its
//         projected area follows the needle's own as the view turns, down to
//         its section end-on;
//       - an aggregate's, the ellipsoid inscribed in the box: a box's
//         projected area along a direction d is c_x|d_x| + c_y|d_y| +
//         c_z|d_z| of the cell's face (c the axes' coverages), up to sqrt(3)
//         times an isotropic cloud's along a diagonal, where the
//         ellipsoid's is sqrt((c_x d_x)^2 + (c_y d_y)^2 + (c_z d_z)^2), the
//         projected area of an SGGX distribution with its axes on the grid's
//         (Heitz et al., "The SGGX Microflake Distribution", SIGGRAPH 2015):
//         the same in every direction for an isotropic cloud, a cosine for a
//         disc.
//   - Its surface as seen from outside: albedo, roughness, metallic, and
//     ambient occlusion averaged over the front-most hits of the coverage
//     rays, and its distribution of normals, a two-sided von Mises-Fisher
//     lobe (an axis and a concentration kappa) fitted to those hits' second
//     moment E[n n^T].
//
// The traversal (nph_render_voxel_trace) is the C twin of the shaders'
// (vis_common.glsl vis_voxel_trace): the cells of a brick a ray crosses, in
// order (Amanatides & Woo, "A Fast Voxel Traversal Algorithm for Ray
// Tracing", Eurographics 1987), each occupied cell's footprint tested. The
// shading terms (nph_render_voxel_diffuse and the others) are the twins of
// the voxel resolve's: the part of the lobe the viewer sees, a spherical
// Gaussian (Wang et al., "All-Frequency Rendering of Dynamic, Spatially-
// Varying Reflectance", SIGGRAPH Asia 2009), whose mean clamped cosine
// toward a light has Hill's fitted closed form (in Pettineo, "SG Series
// Part 3: Diffuse Lighting From an SG Light Source", 2016); the lobe's
// variance added to GGX's (Toksvig, "Mipmapping Normal Maps", JGT 2005;
// Kaplanyan et al., "Filtering Distributions of Normals for Shading
// Antialiasing", HPG 2016); and its convolution of the environment's
// spherical harmonics (Han et al., "Frequency Domain Normal Map Filtering",
// SIGGRAPH 2007).
//
// Thread-safety: the functions are pure (NPH_THREADSAFE).

#include <nepheline/core/types.h>
#include <nepheline/math/vec.h>

#ifdef __cplusplus
extern "C" {
#endif

// The cells a brick spans along each axis, and in all.
#define NPH_RENDER_VOXEL_BRICK       4U
#define NPH_RENDER_VOXEL_BRICK_CELLS 64U
// What a voxel cluster holds at most: the visibility buffer names a brick
// of its cluster in 7 bits, and the shadow map's raster emits a quad a
// brick (render/cluster_lod.h).
#define NPH_RENDER_VOXEL_CLUSTER_BRICKS 32U
#define NPH_RENDER_VOXEL_CLUSTER_VOXELS 1024U
// A footprint's corners are in 32nds of the cell.
#define NPH_RENDER_VOXEL_BOX_STEPS 32U
// The coverage rays along each axis of a cell: a jittered grid of this many
// squared across the cell's face.
#define NPH_RENDER_VOXEL_RAYS 16U
// A footprint's shape within its box (NphRenderVoxel.box's top two bits).
#define NPH_RENDER_VOXEL_SHAPE_SHIFT     30U
#define NPH_RENDER_VOXEL_SHAPE_BOX       0U // the box
#define NPH_RENDER_VOXEL_SHAPE_ELLIPSOID 1U // the ellipsoid inscribed in it
#define NPH_RENDER_VOXEL_SHAPE_CAPSULE                                                             \
    2U // a capsule along one of its diagonals, clipped to the cell
// A capsule's radius, as a fraction of the cell, from its 6-bit code c:
// 2^(c / 8 - 8), from 1/256 (c 0) to 1/2 (c 56, the largest used), each step
// 2^(1/8) = 9 % from the next (nph_render_voxel_radius_encode).
#define NPH_RENDER_VOXEL_RADIUS_CODES 57U

// The largest concentration a voxel's byte holds (2^(255 / 16) - 1, rounded
// down): a lobe that narrow is a flat sheet's.
#define NPH_RENDER_VOXEL_KAPPA_MAX 62756.0F

// A voxel (16 bytes; shaders/shared/nph_vis.h reads it as a uvec4).
typedef struct NphRenderVoxel {
    u32 color;    // albedo r, g, b sRGB-encoded (8 bits each, low first); ambient occlusion
                  // (unorm8) in the top byte
    u32 normal;   // the lobe's axis, octahedral snorm16 x 2 (math/pack.h nph_oct_encode_snorm16)
    u32 material; // roughness unorm8, metallic unorm8, the concentration's byte
                  // (nph_render_voxel_kappa_encode); then a capsule's diagonal (2 bits) and
                  // its radius's code (6 bits), else 0
    u32 box;      // the footprint's box in 32nds of the cell, 5 bits each from the low end: its
                  // low corner x, y, z, then its high corner minus one x, y, z; then its shape
                  // (NPH_RENDER_VOXEL_SHAPE_*)
} NphRenderVoxel;

// A brick (32 bytes; two uvec4 to the shaders): 4 x 4 x 4 cells of edge
// `cell` from `origin`, the occupied ones set in the mask (bit x + 4y + 16z),
// their voxels in bit order from `first_voxel`.
typedef struct NphRenderVoxelBrick {
    NphVec3 origin; // mesh space: the brick's lowest corner
    f32 cell;       // a cell's edge, mesh units (a power of two)
    u32 mask[2];    // occupancy: bits 0..31, then 32..63
    u32 first_voxel;
    u32 reserved; // 0
} NphRenderVoxelBrick;

NPH_STATIC_ASSERT(sizeof(NphRenderVoxel) == 16U, "a voxel is 16 bytes");
NPH_STATIC_ASSERT(sizeof(NphRenderVoxelBrick) == 32U, "a brick is 32 bytes");

// A voxel's contents, unpacked.
typedef struct NphRenderVoxelDesc {
    NphVec3 box_lo; // the footprint's box's corners, as fractions of the cell (32nds)
    NphVec3 box_hi;
    u32 shape; // NPH_RENDER_VOXEL_SHAPE_*
    // A capsule's diagonal: its start is the box's corner (x, y, 0) with x
    // the bit 0 and y the bit 1, its end the opposite corner.
    u32 diagonal;
    f32 radius;     // a capsule's, as a fraction of the cell (1/256 .. 1/2)
    NphVec3 albedo; // linear
    f32 occlusion;
    NphVec3 axis; // the lobe's axis, unit (either way round: the lobe is two-sided)
    f32 kappa;    // its concentration, 0 (isotropic) .. NPH_RENDER_VOXEL_KAPPA_MAX
    f32 roughness;
    f32 metallic;
} NphRenderVoxelDesc;

// Packs `d`, rounding every field to its encoding: the footprint's corners to
// the nearest 32nd (at least one apart on each axis, within [0, 1]), a
// capsule's radius to its nearest code, the albedo to sRGB8, the axis to
// octahedral snorm16, kappa to its byte.
NphRenderVoxel nph_render_voxel_pack(const NphRenderVoxelDesc *d);
void nph_render_voxel_unpack(NphRenderVoxel v, NphRenderVoxelDesc *out);

// A capsule's radius's code: the nearest in log2, held to [0, 56].
u32 nph_render_voxel_radius_encode(f32 radius);
f32 nph_render_voxel_radius_decode(u32 code);

// The concentration's byte: kappa = 2^(b / 16) - 1, the nearest such to
// `kappa` (clamped to [0, NPH_RENDER_VOXEL_KAPPA_MAX]).
u32 nph_render_voxel_kappa_encode(f32 kappa);
f32 nph_render_voxel_kappa_decode(u32 b);
// A two-sided lobe's second moment along its axis, E[(n . axis)^2]: 1/3 for
// kappa 0 (isotropic) rising to 1 (a sheet): 1 - 2 A(kappa) / kappa with
// A(kappa) = coth(kappa) - 1/kappa.
f32 nph_render_voxel_moment(f32 kappa);
// Its inverse over [1/3, 1]: the concentration of the lobe whose second
// moment is `m` (0 at or below 1/3, NPH_RENDER_VOXEL_KAPPA_MAX at 1).
f32 nph_render_voxel_kappa_from_moment(f32 m);

// A ray's first voxel in a brick.
typedef struct NphRenderVoxelHit {
    f32 t;     // along the ray: origin + t dir
    u32 voxel; // index into the voxels (the brick's first_voxel plus the cell's rank)
    u32 cell;  // the cell's bit, x + 4y + 16z
    // The footprint's face it entered: axis (0, 1, 2) | 4 when on the box's
    // high side; for an ellipsoid, the axis of the normal's largest component
    // there, on the side the ray came from.
    u32 face;
} NphRenderVoxelHit;

// The first voxel of `brick` (its voxels in `voxels`) that the ray origin +
// t dir meets for t in [t_min, t_max], the footprints tested in the order
// the ray crosses their cells. False when it meets none. A ray that starts
// inside a footprint meets it at t_min. `dir` need not be unit; a zero
// component is taken as a tiny one of positive sign, as the shaders do.
bool nph_render_voxel_trace(const NphRenderVoxelBrick *brick, const NphRenderVoxel *voxels,
                            NphVec3 origin, NphVec3 dir, f32 t_min, f32 t_max,
                            NphRenderVoxelHit *out);

// The shading model (the voxel resolve's twin; docs/render.md "Voxels").
// For the lobe facing the viewer, axis `n` (unit, n . v >= 0) and
// concentration `kappa`, with the viewer in unit direction `v`:
//
// The visible lobe, the normals the viewer sees, D(m) (m.v)+ normalised: the
// vMF lobe times the viewer's cosine taken as a spherical Gaussian of
// sharpness NPH_RENDER_VOXEL_VIEW_SHARPNESS, itself a vMF lobe, of axis
// (kappa n + s v) / |kappa n + s v| (written to `*out_axis`) and the
// concentration returned, |kappa n + s v|.
#define NPH_RENDER_VOXEL_VIEW_SHARPNESS 2.8F
f32 nph_render_voxel_visible(NphVec3 n, f32 kappa, NphVec3 v, NphVec3 *out_axis);
// The diffuse response to a light in unit direction `l`: the clamped cosine
// toward it averaged over the visible lobe, by Hill's fit, which is the
// cosine (n.l)+ itself for a lobe of one normal (kappa large).
f32 nph_render_voxel_diffuse(NphVec3 n, f32 kappa, NphVec3 v, NphVec3 l);
// GGX's alpha squared with the lobe's spread added: alpha^2 + 2 / kappa (a
// vMF lobe's slopes have variance 1 / kappa about each tangent; Beckmann's
// m^2 is twice the slopes' variance, and GGX's alpha is taken as m), at most
// 1.
f32 nph_render_voxel_alpha2(f32 alpha2, f32 kappa);
// The factors the lobe's convolution scales the spherical harmonics' bands
// 1 and 2 by: A(kappa) and 1 - 3 A(kappa) / kappa (the means of the Legendre
// polynomials P1 and P2 over a vMF lobe).
f32 nph_render_voxel_sh_band1(f32 kappa);
f32 nph_render_voxel_sh_band2(f32 kappa);

// ---------------------------------------------------------------------------
// The material sampler the level builder voxelizes with
// ---------------------------------------------------------------------------

// How a submesh's surface takes part in voxels.
typedef enum NphRenderVoxelSurface {
    NPH_RENDER_VOXEL_SURFACE_NONE = 0,   // never voxelized (blended, emissive): its groups stay
                                         // triangles
    NPH_RENDER_VOXEL_SURFACE_OPAQUE = 1, // every point covers
    NPH_RENDER_VOXEL_SURFACE_MASKED = 2, // the sampler's `covers` decides each point
} NphRenderVoxelSurface;

// A surface's look at a point, as its material has it there.
typedef struct NphRenderVoxelSample {
    NphVec3 base_color; // linear, times the vertex colour
    NphVec3 normal;     // the shading normal (with the normal map), mesh space, unit
    f32 roughness;      // perceptual
    f32 metallic;
    f32 occlusion; // the material's ambient occlusion (1: none)
} NphRenderVoxelSample;

// The caller's materials (cluster_lod.h NphRenderClusterLodInput.voxels).
// Both functions take a point by its triangle's three mesh vertices `v` and
// barycentrics `b` (weights of v[0], v[1], v[2]), of a triangle of submesh
// (input range) `submesh`, and `footprint`, the spacing in mesh units of the
// points sampled around it, which texture lookups filter over. They must be
// NPH_THREADSAFE: the builder may call them from several threads at once.
typedef struct NphRenderVoxelSampler {
    void *user;
    const u8 *submesh_surface; // per submesh (input range): NphRenderVoxelSurface
    void (*sample)(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint,
                   NphRenderVoxelSample *out);
    // Whether a masked submesh's surface covers the point (its alpha against
    // its cutoff). Called only for NPH_RENDER_VOXEL_SURFACE_MASKED.
    bool (*covers)(void *user, u32 submesh, const u32 v[3], NphVec3 b, f32 footprint);
} NphRenderVoxelSampler;

#ifdef __cplusplus
}
#endif

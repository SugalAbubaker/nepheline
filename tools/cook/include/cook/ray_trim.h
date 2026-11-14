// SPDX-License-Identifier: Apache-2.0
#pragma once

// Triangles for the rays alone (asset/mesh.h version 7; docs/cook.md "Rays";
// docs/adr/0096-leaf-cards-trimmed-for-rays.md).
//
// A ray that meets an alpha-tested triangle is handed it as a candidate wherever it meets it,
// and the shader reads the triangle's texture to learn that the ray goes on (rt_mask.glsl): on a
// tree's cards, most of whose area is cut away, most of what a ray pays for (Gruen, Benthin and
// Woop, "Sub-triangle opacity masks for faster ray tracing of transparent objects", HPG 2020,
// measure the same cost). Here each such triangle is cut down, for the rays alone, to a convex
// polygon that holds every point of it where the test can pass, as a particle's quad is trimmed
// to its sprite for the raster (Persson, "Particle trimmer", 2009; "Graphics gems for games:
// findings from Avalanche Studios", SIGGRAPH 2012): the triangle less the half planes that hold
// none of that. The raster's triangles stay as they are.
//
// Where the test can pass is bounded from the cooked texture itself, as the GPU decodes it. The
// rays read its finest level at a point (rt_mask.glsl), and a bilinear lookup gives no more than
// the greatest of the four texels about the point: so a texel whose alpha can pass (with the
// greatest alpha the triangle's vertices and the material's factor allow) holds the lookups
// within half a texel beyond its sides, and nothing else passes. COOK_RAY_TRIM_MARGIN more is
// kept for the arithmetic: the shader's weights and texture coordinates are floats, and a
// polygon's corners are rounded to the 65535ths of its triangle they are stored as.
//
// A polygon's corners on an edge of its triangle are made the same points in every triangle
// that shares the edge and reaches it (cook_ray_trim_build), and the renderer places such a
// corner from the edge's ends alone (render/rt.c): two neighbours' polygons then share whole
// edges, bit for bit, and no ray passes between them where both let none through.
//
// Thread-safety: pure functions over the caller's data; NPH_THREADSAFE on distinct outputs.

#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/core/array.h>
#include <nepheline/math/vec.h>

#include <cook/cook.h>

#ifdef __cplusplus
extern "C" {
#endif

// The half planes a triangle may lose, and the directions their edges are chosen from.
#define COOK_RAY_TRIM_CUTS       4U
#define COOK_RAY_TRIM_DIRECTIONS 32U
// A cut takes at least this share of its triangle's area, and a submesh is trimmed where at
// least this share of its area goes: under it the candidates saved do not pay for the polygons'
// triangles (measured: benchmarks/results, ADR-0096).
#define COOK_RAY_TRIM_MIN_CUT   0.02F
#define COOK_RAY_TRIM_MIN_SHARE 0.25F
// Texels kept beyond a texel's reach.
#define COOK_RAY_TRIM_MARGIN 0.25
// A polygon's corners: three, one more a cut, and those its neighbours' polygons add on its
// edges (two an edge at most in all, and no more than a cut each within).
#define COOK_RAY_TRIM_MAX_CORNERS 12U
// A triangle longer than this in its texture's texels stays whole: a corner's rounding would
// outrun the margin. So does one that spans more than COOK_RAY_TRIM_MAX_REPEATS of the texture
// on either axis (it meets every texel, and little of it can go), which bounds a triangle's
// work by its texture's size; and a submesh whose triangles' texels come to more than
// COOK_RAY_TRIM_MAX_WALK is left as it is, which bounds a cook's.
#define COOK_RAY_TRIM_MAX_TEXELS  8192.0
#define COOK_RAY_TRIM_MAX_REPEATS 2.0
#define COOK_RAY_TRIM_MAX_WALK    (1ULL << 30U)

// A texture's alpha as the rays' test reads it: its finest level as cooked, rows from the top.
typedef struct CookRayAlpha {
    const u8 *texels;
    u32 width;
    u32 height;
} CookRayAlpha;

// What a material slot's alpha test reads. `key` is the material's
// (NphMaterialHeader.alpha_key); 0: the slot's triangles are not trimmed.
typedef struct CookRayTrimSlot {
    CookRayAlpha alpha;
    u32 wrap_u; // NphMaterialWrap
    u32 wrap_v;
    f32 factor; // the base colour factor's alpha
    f32 cutoff;
    u64 key;
} CookRayTrimSlot;

typedef struct CookRayTrimSlots {
    const CookRayTrimSlot *slots; // a material slot each
    u32 count;
} CookRayTrimSlots;

// A point of a triangle: the weights of the triangle's second and third corners in 65535ths
// (the rest is its first corner's).
typedef struct CookRayCorner {
    u16 second;
    u16 third;
} CookRayCorner;

// A triangle's polygon: its corners in the triangle's own turn, no three in a line. `count` 0:
// no point of the triangle can pass the test.
typedef struct CookRayPolygon {
    u32 count;
    CookRayCorner corner[COOK_RAY_TRIM_MAX_CORNERS];
} CookRayPolygon;

typedef struct CookRayTrimStats {
    u32 submeshes;   // trimmed
    u32 left;        // alpha tested with a key, and left as they are
    u32 parents;     // the trimmed submeshes' triangles
    u32 dropped;     // of those, the ones no ray can pass the test in
    u32 distinct;    // triangles the test tells apart, each worked out once
    u32 edge_passes; // passes that made shared edges' corners agree
    f64 area;        // the trimmed submeshes' area, mesh space
    f64 area_kept;   // and their polygons'
} CookRayTrimStats;

typedef NPH_ARRAY(NphMeshRayRange) CookRayRangeArray;
typedef NPH_ARRAY(NphMeshRayTriangle) CookRayTriangleArray;

// A mesh's triangles for the rays: `ranges` a submesh each and `triangles` (the asset's RAYR
// and RAYT), both empty where no submesh is trimmed.
typedef struct CookRayTrim {
    CookRayRangeArray ranges;
    CookRayTriangleArray triangles;
    CookRayTrimStats stats;
} CookRayTrim;

void cook_ray_trim_init(CookRayTrim *trim, NphAllocator *alloc);
void cook_ray_trim_free(CookRayTrim *trim);

// The key of an alpha test (NphMaterialHeader.alpha_key): a hash of the texels and of what
// else the test reads, never 0.
u64 cook_ray_trim_key(const CookRayAlpha *alpha, u32 wrap_u, u32 wrap_v, bool nearest, f32 factor,
                      f32 cutoff);

// Whether the test passes at texture coordinates (u, v) with the vertices' alpha
// `vertex_alpha` there, as the rays' shader evaluates it with a bilinear lookup (the reference
// the polygons are tested against).
bool cook_ray_trim_passes(const CookRayTrimSlot *slot, f32 u, f32 v, f32 vertex_alpha);

// The polygon of a triangle whose three corners' texture coordinates are `uv` and whose greatest
// vertex alpha is `vertex_alpha`: the whole triangle where nothing can be cut (or the test
// cannot be bounded: no area in the texture, or too long in it), none where no point can pass.
// Returns the polygon's share of the triangle's area.
f32 cook_ray_trim_triangle(const CookRayTrimSlot *slot, const NphVec2 *uv, f32 vertex_alpha,
                           CookRayPolygon *out);

// The mesh's triangles for the rays, into `out` (initialised): each submesh whose slot has a
// key, and of whose area at least COOK_RAY_TRIM_MIN_SHARE goes, as its triangles' polygons,
// fans from their first corners. `positions`, `attribs`, `indices` and `submeshes` are the
// cooked mesh's (level 0); `position_scale` its quantisation box's size, for the areas.
// NPH_RESULT_OUT_OF_MEMORY; NPH_RESULT_INVALID_ARGUMENT for a submesh range or index out of
// bounds.
NPH_MUST_USE NphResult cook_ray_trim_build(NphAllocator *alloc, const NphMeshPosition *positions,
                                           const NphMeshAttrib *attribs, u32 vertex_count,
                                           const u32 *indices, u32 index_count,
                                           const NphSubmesh *submeshes, u32 submesh_count,
                                           NphVec3 position_scale, const CookRayTrimSlots *slots,
                                           CookRayTrim *out);

#ifdef __cplusplus
}
#endif

// SPDX-License-Identifier: Apache-2.0
// The GPU-driven visibility-buffer renderer's records (engine/render vis*.c;
// docs/render.md "The production path"; ADR-0029), shared by its C code
// and its shaders (shaders/render/vis_*). Everything is reached through
// buffer addresses held in one NphVisViewData per frame; push constants
// carry that address and a pass's own arguments.
//
// The scene lives on the GPU: instances, meshes, and the per-instance
// material map are persistent tables indexed by pool slot and updated only
// where something changed, so the CPU's cost per frame does not grow with
// the object count (CLAUDE.md §8.6.3, §12 Phase 10). Positions are 64-bit
// on the CPU; a table stores each translation as a high and a low float
// whose difference from the camera's own split is taken on the GPU, so
// precision holds at any distance from the origin ("GPU RTE", Cozzi &
// Ring, "3D Engine Design for Virtual Globes", 2011, §5.4).
#ifndef NPH_VIS_SHARED_H
#define NPH_VIS_SHARED_H

#include "nph_render.h"

// Instance flags (NphVisInstance.flags).
#define NPH_VIS_INSTANCE_VISIBLE  1 // live and not hidden: culled and drawn
#define NPH_VIS_INSTANCE_MIRRORED 2 // the linear part's determinant is negative
#define NPH_VIS_INSTANCE_FRESH    4 // created this frame: the previous transform is the current one
#define NPH_VIS_INSTANCE_SIMILAR                                                                   \
    8 // rotation, uniform scale, mirroring only: angles kept (cone culling)
// Its transform is not the previous frame's (the scatter pass keeps it):
// motion vectors read both.
#define NPH_VIS_INSTANCE_MOVED 16

// Cluster flags (NphVisCluster.flags), from the instance and the material.
#define NPH_VIS_CLUSTER_MIRRORED     1
#define NPH_VIS_CLUSTER_DOUBLE_SIDED 2
#define NPH_VIS_CLUSTER_MASKED       4
#define NPH_VIS_CLUSTER_MOVED        8 // its instance's NPH_VIS_INSTANCE_MOVED

// The visibility buffer: 64 bits per pixel, the depth's IEEE bits high
// (reverse-Z, so the largest value is the nearest and atomicMax is the
// depth test), then the visible cluster's index and the triangle within
// its meshlet (Burns & Hunt, "The Visibility Buffer", JCGT 2013; the
// packing of Karis et al., "Nanite: A Deep Dive", SIGGRAPH 2021). Zero is
// "nothing drawn": the far plane, which no surface reaches.
#define NPH_VIS_TRIANGLE_BITS 7
#define NPH_VIS_TRIANGLE_MASK 127
#define NPH_VIS_MAX_CLUSTERS  33554432 // 2^25

// NphVisCounters.overflow bits: a list was full and items were dropped.
#define NPH_VIS_OVERFLOW_MESHLETS 1 // phase 0's meshlet candidates (max_cull_meshlets)
#define NPH_VIS_OVERFLOW_CLUSTERS 2
#define NPH_VIS_OVERFLOW_DEFERRED 4

// Raster lists, per phase: how each visible cluster is rasterised.
#define NPH_VIS_RASTER_OPAQUE   0 // hardware
#define NPH_VIS_RASTER_MASKED   1 // hardware, alpha tested
#define NPH_VIS_RASTER_SOFTWARE 2 // compute (vis_sw_raster.comp): small opaque clusters
#define NPH_VIS_RASTER_KINDS    3

// The depth pyramid after phase 1 rebuilds only the tiles of this many
// pixels square that phase 1 drew into (vis_hzb.comp mode 2).
#define NPH_VIS_HZB_TILE 32

// The widest cluster, in pixels, the software raster takes: it keeps every
// triangle's box small enough for 32-bit edge functions (vis_sw_raster.comp).
#define NPH_VIS_SW_MAX_PIXELS 64

// NphVisViewData.flags.
#define NPH_VIS_VIEW_OCCLUSION    1  // test against the hierarchical depth
#define NPH_VIS_VIEW_HZB_VALID    2  // the previous frame's HZB matches this view's target
#define NPH_VIS_VIEW_MESH_SHADERS 4  // rasterise with mesh shaders (else indirect draws)
#define NPH_VIS_VIEW_CONE_CULLING 8  // meshlet normal-cone backface culling
#define NPH_VIS_VIEW_FREEZE       16 // keep last frame's culling camera (debugging)
#define NPH_VIS_VIEW_SW_RASTER    32 // small clusters rasterise in software
#define NPH_VIS_VIEW_LIGHT_GRID   64 // local lights are binned (else every light is global)

// vis_lights.comp modes (NphVisPush.mode).
#define NPH_VIS_LIGHTS_UPDATE 0 // apply the frame's light updates
#define NPH_VIS_LIGHTS_CULL   1 // the lights in view, binned by distance

// vis_args.comp modes (NphVisPush.mode).
#define NPH_VIS_ARGS_PHASE1      0 // after phase 0's instance cull: its meshlet cull
#define NPH_VIS_ARGS_RASTER1     1 // after phase 0's meshlet cull: its rasters
#define NPH_VIS_ARGS_PHASE2      2 // before phase 1: instance cull of the deferred instances
#define NPH_VIS_ARGS_PHASE2_MESH 3 // after phase 1's instance cull: its meshlet cull
#define NPH_VIS_ARGS_RASTER2     4 // after phase 1's meshlet cull: its rasters

// A transform: the linear part's rows in xyz, the translation's high
// float in the rows' w, and its low float in `translation_low`.
NPH_SHARED_STRUCT(NphVisTransform)
{
    NphVec4 row0;
    NphVec4 row1;
    NphVec4 row2;
    NphVec4 translation_low; // xyz; w: unused
};

// One instance, at its pool slot.
NPH_SHARED_STRUCT(NphVisInstance)
{
    NphVisTransform transform;
    u32 mesh;           // mesh table index (its pool slot)
    u32 material_base;  // first entry of its material slots in the material map
    u32 material_count; // entries (material slots)
    u32 flags;          // NPH_VIS_INSTANCE_*
};

// An instance update the scatter pass applies at the start of a frame:
// `instance` replaces the record at `slot`, whose old transform becomes
// the previous one. With `copy_previous` set the record is not touched and
// the previous transform catches up with the current one (an instance
// that moved last frame and not this one).
NPH_SHARED_STRUCT(NphVisInstanceUpdate)
{
    u32 slot;
    u32 copy_previous;
    u32 pad0;
    u32 pad1;
    NphVisInstance instance;
};

// One light, at its pool slot: the record the shaders light with, its
// position split like an instance's translation (the light pass makes it
// camera relative).
NPH_SHARED_STRUCT(NphVisLight)
{
    NphRenderLightData data; // data.position.xyz: the 64-bit position's high floats
    NphVec4 position_low;    // xyz: its low floats; w: the range (0: unlimited)
    u32 live;                // 0: a free slot
    u32 pad0;
    u32 pad1;
    u32 pad2;
};

// A light update the light pass applies at the start of a frame.
NPH_SHARED_STRUCT(NphVisLightUpdate)
{
    u32 slot;
    u32 pad0;
    u32 pad1;
    u32 pad2;
    NphVisLight light;
};

// One mesh, at its pool slot: dequantisation, bounds, and its streams.
NPH_SHARED_STRUCT(NphVisMesh)
{
    NphVec4 position_offset; // xyz: dequantise as offset + unorm16 / 65535 * scale
    NphVec4 position_scale;  // xyz
    NphVec4 sphere;          // xyz centre, w radius, in mesh space
    u64 positions;           // NphRenderVertexPosition[]
    u64 attribs;             // NphRenderVertexAttrib[]
    u64 meshlets;            // NphRenderMeshlet[] (16 bytes each)
    u64 meshlet_bounds;      // NphRenderMeshletBounds[] (32 bytes each)
    u64 meshlet_vertices;    // u32[]: mesh vertex indices
    u64 meshlet_triangles;   // u8[3 * triangles] read as u32 words
    u64 submesh_slots;       // u32[submesh]: each submesh's material slot
    u32 meshlet_count;
    u32 index_base; // the mesh's first corner in the meshlet index pool (indirect raster)
    u64 blas;       // its bottom-level acceleration structure, over the quantised positions
                    // (0: none)
    u32 pad0;
    u32 pad1;
};

// A visible cluster: one meshlet of one instance that survived culling.
NPH_SHARED_STRUCT(NphVisCluster)
{
    u32 instance;
    u32 meshlet;
    u32 material; // material table index (the default material when the slot names none)
    u32 flags;    // NPH_VIS_CLUSTER_*
};

// VkDrawIndexedIndirectCommand: one meshlet of the indirect raster.
NPH_SHARED_STRUCT(NphVisDraw)
{
    u32 index_count;
    u32 instance_count;
    u32 first_index;
    i32 vertex_offset;
    u32 first_instance;
};

// The indirect arguments vis_args.comp derives from the counters, in a
// buffer of their own (read as indirect arguments while the counters are
// written).
NPH_SHARED_STRUCT(NphVisArgs)
{
    u32 cull_args[2][4];      // meshlet cull per phase: x, y, z, unused
    u32 instance2_args[4];    // phase 1's cull of the instances phase 0 deferred
    u32 raster_args[2][3][4]; // [phase][NPH_VIS_RASTER_*]: mesh-task or compute groups
    u32 light_tile_args[4];   // the light masks: tile groups in x and y, light words in z
};

// The frame's counters (cleared at the start of every frame).
NPH_SHARED_STRUCT(NphVisCounters)
{
    u32 cull_meshlets;      // phase 0's meshlet candidates
    u32 deferred_instances; // instances phase 0 found occluded
    u32 deferred_meshlets;  // phase 1's meshlet candidates: those phase 0 deferred, then the
                            // meshlets of deferred instances phase 1 found visible
    u32 clusters;           // visible clusters, phase 0's first
    u32 raster_count[2][3]; // [phase][NPH_VIS_RASTER_*]: clusters to rasterise
    u32 overflow;           // NPH_VIS_OVERFLOW_*
    u32 hzb_full;           // nonzero: phase 1 drew something no tile list bounds, so the
                            // pyramid after it rebuilds every tile
    u32 lights_global;      // NphRenderLightGrid.counts: lights that reach every pixel
    u32 lights_local;       // and the others in view
    // Statistics, read back two frames later.
    u32 instances_culled_frustum;
    u32 instances_culled_occlusion; // after phase 2
    u32 meshlets_culled_frustum;
    u32 meshlets_culled_cone;
    u32 meshlets_culled_occlusion; // after phase 2
    u32 triangles;                 // in the visible clusters
    u32 rt_instances;              // the top level's active instance records (rt_instances.comp)
};

// The production path's view of a frame.
NPH_SHARED_STRUCT(NphVisViewData)
{
    NphMat4 view;           // camera-relative world -> view (the camera's rotation)
    NphMat4 view_proj;      // camera-relative world -> clip
    NphMat4 prev_view;      // the previous frame's, relative to its own camera
    NphMat4 prev_view_proj; // the previous frame's, relative to its camera, without its jitter
                            // (motion vectors)
    // A still point's motion: this frame's clip space (with its jitter, as
    // drawn) to the previous frame's, through the camera's move since:
    // prev_view_proj * translate(camera - prev_camera) * inverse(view_proj).
    NphMat4 reproject;
    NphVec4 frustum[5];  // camera-relative inward planes (xyz normal, w distance): l, r, b, t, near
    NphVec4 camera_high; // xyz: the camera position's high floats
    NphVec4 camera_low;  // xyz: and its low floats
    NphVec4 prev_camera_high;
    NphVec4 prev_camera_low;
    NphVec4 viewport;   // width, height, 1 / width, 1 / height
    NphVec4 projection; // x: P00, y: P11, z: near plane distance, w: unused
    NphVec4 prev_projection;
    NphVec4 hzb; // x, y: the HZB's mip-0 extent, z: its mip count, w: unused
    // xy: this frame's sub-pixel offset of the projection (temporal
    // anti-aliasing) in UV, zw: the previous frame's: rectangles projected
    // with `projection` or `prev_projection` move by it.
    NphVec4 jitter;
    u64 render_view;        // NphRenderViewData: lights, materials, environment
    u64 instance_updates;   // NphVisInstanceUpdate[] of this frame (the scatter pass)
    u64 instances;          // NphVisInstance[]
    u64 prev_transforms;    // NphVisTransform[]
    u64 meshes;             // NphVisMesh[]
    u64 material_map;       // u32[]: material handle values
    u64 materials;          // NphRenderMaterialData[]
    u64 counters;           // NphVisCounters
    u64 args;               // NphVisArgs
    u64 cull_meshlets;      // uvec2[] (instance, meshlet): phase 0's meshlet candidates
    u64 deferred_instances; // u32[]
    u64 deferred_meshlets;  // uvec2[] (instance, meshlet): phase 1's meshlet candidates
    u64 clusters;           // NphVisCluster[]
    u64 raster_lists[2][3]; // u32[] of cluster indices: [phase][NPH_VIS_RASTER_*]
    u64 draws[2][2];        // NphVisDraw[]: the hardware kinds, for the indirect raster
    u64 hzb_dirty;          // u32[]: a bit per NPH_VIS_HZB_TILE tile phase 1 drew into
    u64 lights;             // NphVisLight[]
    u64 light_updates;      // NphVisLightUpdate[] of this frame
    u64 light_grid;         // NphRenderLightGrid: what the shading reads
    u64 light_histogram;    // u32[NPH_RENDER_ZBINS]: local lights per nearest-distance bin
    u64 light_visible;      // uvec4[]: per local light in view, its bins (nearest | farthest <<
                            // 16), then its tiles (x0 | y0 << 16, x1 | y1 << 16)
    u64 light_staged;       // NphRenderLightData[]: those lights, camera relative, unsorted
    u64 light_rects;        // uvec2[]: their tiles, sorted like the grid's locals
    u32 instance_count;     // slots the instance cull walks
    u32 max_cull_meshlets;
    u32 max_deferred;
    u32 max_clusters;
    u32 hzb_index;           // bindless sampled image: the HZB
    u32 material_index_mask; // material handle -> table index
    u32 default_material;    // the table index of the default material
    u32 flags;               // NPH_VIS_VIEW_*
    u32 max_mesh_groups_x;   // the device's mesh-task workgroup count limit in x
    f32 sw_raster_pixels;    // clusters whose triangles average at most this many pixels
                             // across rasterise in software (NPH_VIS_VIEW_SW_RASTER)
    u32 hzb_tiles_x;         // tiles per row of hzb_dirty
    u32 light_slots;         // slots the light pass walks
    u32 light_tiles_y;       // rows of light tiles (the grid has the row length)
    u32 pad0;
};

// Push constants of every production-path shader. The visibility buffer
// and the HDR target are render-graph transients, so their bindless
// indices are only known while a pass records: they travel here, not in
// the view.
// The ambient occlusion's index on a frame without it: the resolve's
// (NphVisPush.arg2) and the depth pyramid's (arg4).
#define NPH_VIS_NO_AO 0xFFFFFFFFU

NPH_SHARED_STRUCT(NphVisPush)
{
    u64 view;  // NphVisViewData
    u32 phase; // 0 or 1
    u32 mode;  // pass-specific (vis_args.comp: NPH_VIS_ARGS_*)
    u32 arg0;  // pass-specific: see each shader
    u32 arg1;
    u32 arg2;
    u32 arg3;
    u32 arg4;
};

// The culling math against its C twins (render_tests, vis_math_test.comp,
// render/reference.h): one case per invocation.
NPH_SHARED_STRUCT(NphVisMathCase)
{
    NphVec4 sphere; // centre (view space for the projection, else camera relative), radius
    NphVec4 cone;   // apex (camera relative), cutoff in w
    NphVec4 axis;   // cone axis
    NphVec4 c0;     // a triangle's clip-space vertices
    NphVec4 c1;
    NphVec4 c2;
    NphVec4 rect; // an HZB query rectangle (UV)
    // A light (vis_light_sphere): position and range; direction and
    // NPH_RENDER_LIGHT_*; the cosine and sine of the outer cone angle, and
    // in z a view distance for vis_zbin.
    NphVec4 light_position;
    NphVec4 light_direction;
    NphVec4 light_cone;
    f32 depth; // the rectangle's nearest depth
    u32 mirrored;
    u32 pad0;
    u32 pad1;
};

NPH_SHARED_STRUCT(NphVisMathResult)
{
    NphVec4 rect;         // vis_project_sphere
    NphVec4 clipped_rect; // vis_project_sphere_clipped
    NphVec4 light_sphere; // vis_light_sphere
    f32 depth;
    u32 projected;
    u32 in_frustum;   // vis_sphere_in_frustum
    u32 cone_culled;  // vis_cone_culled
    u32 front_facing; // vis_front_facing
    u32 tri_culled;   // vis_cull_triangle (back faces culled)
    u32 hzb_occluded; // vis_hzb_occluded
    u32 zbin;         // vis_zbin
};

NPH_SHARED_STRUCT(NphVisMathPush)
{
    u64 view;    // NphVisViewData: frustum, viewport, projection, hzb, hzb_index
    u64 cases;   // NphVisMathCase[]
    u64 results; // NphVisMathResult[]
    u32 count;
    u32 pad0;
};

#endif

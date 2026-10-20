// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal state of the renderer (renderer.c, frame.c, ibl.c, post.c,
// dof.c, debug_draw.c, and the production path in vis.c, vis_scene.c,
// vsm.c, taa.c, and motion_blur.c). See
// render/renderer.h for the contract and docs/render.md for the design.

#include <nepheline/core/array.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/handle.h>
#include <nepheline/core/range_alloc.h>
#include <nepheline/gpu/accel.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/render/meshlet.h>
#include <nepheline/render/renderer.h>

#include <nph_render.h>
#include <nph_vis.h>
#include <nph_vsm.h>

#define RENDER_NAME_MAX 64U

// The formats of the renderer's own images. The production path's HDR
// target is B10G11R11 where the device can store to it, sample it, and
// blend into it (half the bytes), else this one (RenderVis.hdr_format).
#define RENDER_HDR_FORMAT   NPH_GPU_FORMAT_R16G16B16A16_SFLOAT
#define RENDER_HDR_COMPACT  NPH_GPU_FORMAT_B10G11R11_UFLOAT
#define RENDER_DEPTH_FORMAT NPH_GPU_FORMAT_D32_SFLOAT
#define RENDER_IBL_FORMAT   NPH_GPU_FORMAT_R16G16B16A16_SFLOAT
// The production path's: the visibility buffer and the hierarchical depth.
#define RENDER_VIS_FORMAT NPH_GPU_FORMAT_R64_UINT
#define RENDER_HZB_FORMAT NPH_GPU_FORMAT_R32_SFLOAT

// The SH projection reads the environment level whose faces are this wide
// (or level 0 when the environment is smaller): 6 x 32 x 32 texels is far
// more than band 2 needs and one workgroup walks them in microseconds.
#define RENDER_SH_SOURCE_SIZE 32U

// Debug-line buffers rotate through one more than the frames in flight so
// the one being filled is never read by the GPU (debug_draw.h).
#define RENDER_DEBUG_BUFFERS (NPH_GPU_FRAMES_IN_FLIGHT + 1U)

// Pipelines whose attachments are the caller's target are kept per target
// format (a swapchain format, a test's offscreen format).
#define RENDER_TARGET_FORMATS 4U

typedef enum RenderShader {
    RENDER_SHADER_FORWARD_VERT = 0,
    RENDER_SHADER_FORWARD_OPAQUE,
    RENDER_SHADER_FORWARD_MASK,
    RENDER_SHADER_FORWARD_BLEND,
    RENDER_SHADER_FULLSCREEN_VERT,
    RENDER_SHADER_SKY_FRAG,
    RENDER_SHADER_TONEMAP_FRAG,
    RENDER_SHADER_DEBUG_VERT,
    RENDER_SHADER_DEBUG_FRAG,
    RENDER_SHADER_IBL_SKY,
    RENDER_SHADER_IBL_EQUIRECT,
    RENDER_SHADER_IBL_CUBE,
    RENDER_SHADER_IBL_DOWNSAMPLE,
    RENDER_SHADER_IBL_PREFILTER,
    RENDER_SHADER_IBL_SH,
    RENDER_SHADER_BRDF_LUT,
    // With ray-traced shadows (rt.c), loaded where the device has ray
    // queries.
    RENDER_SHADER_FORWARD_OPAQUE_RT,
    RENDER_SHADER_FORWARD_MASK_RT,
    RENDER_SHADER_FORWARD_BLEND_RT,
    RENDER_SHADER_RT_INSTANCES,
    RENDER_SHADER_COUNT
} RenderShader;

typedef enum RenderCompute {
    RENDER_COMPUTE_IBL_SKY = 0,
    RENDER_COMPUTE_IBL_EQUIRECT,
    RENDER_COMPUTE_IBL_CUBE,
    RENDER_COMPUTE_IBL_DOWNSAMPLE,
    RENDER_COMPUTE_IBL_PREFILTER,
    RENDER_COMPUTE_IBL_SH,
    RENDER_COMPUTE_BRDF_LUT,
    RENDER_COMPUTE_COUNT
} RenderCompute;

// Face culling of a forward draw: the material's sidedness and whether
// the instance's transform mirrors (which turns counter-clockwise front
// faces clockwise on screen). Double-sided draws cull nothing but still
// need the right front face: the shading flips the normal on back faces.
typedef enum RenderCull {
    RENDER_CULL_BACK_CCW = 0,
    RENDER_CULL_BACK_CW = 1,
    RENDER_CULL_NONE = 2,
    RENDER_CULL_NONE_CW = 3,
    RENDER_CULL_COUNT = 4
} RenderCull;

typedef struct RenderTargetPipelines {
    NphGpuFormat format; // UNDEFINED: unused entry
    NphGpuPipeline tonemap;
    NphGpuPipeline debug_depth;
    NphGpuPipeline debug_overlay;
    // The production path's: no depth attachment (the debug lines test
    // against the visibility buffer).
    NphGpuPipeline vis_tonemap;
    NphGpuPipeline vis_debug_depth;
    NphGpuPipeline vis_debug_overlay;
    // The production path's temporal resolve (taa.c): into the target and
    // the next history (RenderVis.hdr_format).
    NphGpuPipeline taa;
    // The production path's motion blur fused with the tonemap
    // (motion_blur.c).
    NphGpuPipeline motion_blur;
} RenderTargetPipelines;

typedef struct RenderTexture {
    u32 handle; // the live handle's value; 0 when the slot is free
    NphGpuImage image;
    NphRenderTextureKind kind;
    u32 flags;
    u32 width;
    u32 mip_count;
} RenderTexture;

typedef struct RenderMesh {
    u32 handle;
    NphGpuBuffer positions;
    NphGpuBuffer attribs;
    NphGpuBuffer indices;
    u64 positions_address;
    u64 attribs_address;
    NphRenderSubmesh *submeshes; // from the renderer's allocator
    u32 submesh_count;
    u32 index_count;
    NphVec3 position_offset;
    NphVec3 position_scale;
    NphAABB bounds;
    u32 instances; // live instances referencing the mesh
    u32 vertex_count;
    // Ray-traced shadows (rt.c): the bottom-level structure over the
    // quantised positions, the float copy of them it is built from where the
    // device cannot build from 16-bit unorm, and the scratch its build needs.
    NphGpuAccel blas;
    NphGpuBuffer blas_vertices;
    u64 blas_scratch;
    b32 blas_pending; // listed in RenderRt.pending
    // The production path (vis_scene.c): the meshlet streams and the
    // mesh's corners in the index pool (indirect raster only).
    NphGpuBuffer meshlets;
    NphGpuBuffer meshlet_bounds;
    NphGpuBuffer meshlet_vertices;
    NphGpuBuffer meshlet_triangles;
    NphGpuBuffer submesh_table; // NphVisMesh.submeshes
    NphRange index_range;
    u32 meshlet_count;
    u32 meshlet_triangle_count;
    NphSphere sphere; // mesh space
} RenderMesh;

typedef struct RenderMaterial {
    u32 handle;
    NphRenderMaterialDesc desc; // name cleared; handles resolved per frame
} RenderMaterial;

// RenderVis.slot_state bits, per instance slot (they outlive an instance:
// a slot destroyed and reused within one frame is sent once).
typedef enum RenderSlotState {
    RENDER_SLOT_DIRTY = 1U << 0U, // its record must reach the GPU
    RENDER_SLOT_FRESH = 1U << 1U, // created since its record last reached the GPU
    RENDER_SLOT_SENT = 1U << 2U,  // sent last frame: its previous transform must catch up
} RenderSlotState;

typedef struct RenderInstance {
    u32 handle;
    NphRenderMesh mesh;
    NphRenderMaterial *materials; // from the renderer's allocator
    u32 material_count;
    NphRenderTransform transform;
    b32 hidden;
    // The production path.
    NphRange material_range; // its entries in the material map
    u32 blend_index;         // place in the blend list; NPH_U32_MAX: not listed
} RenderInstance;

// The virtual shadow map's work buffer (vsm.c).
typedef struct VsmLayout {
    u64 counters;    // NphVsmCounters, then the three bitmasks: cleared each frame
    u64 requests;    // u32[NPH_VSM_LEVELS * NPH_VSM_LEVEL_WORDS]
    u64 dirty;       // u32[...]
    u64 render;      // u32[...]
    u64 render_rows; // u32[NPH_VSM_LEVELS * NPH_VSM_ROW_WORDS]
    u64 clear_size;  // bytes cleared at the start of a frame
    u64 args;        // NphVsmArgs
    u64 alloc;       // u32[NPH_VSM_LEVELS * NPH_VSM_LEVEL_ENTRIES]
    u64 free;        // u32[vsm_pages]
    u64 candidate;   // u32[vsm_pages]
    u64 rendered;    // u32[vsm_pages]
    u64 meshlets;    // uvec2[max_vsm_meshlets]
    u64 draws;       // uvec4[max_vsm_draws]
    u64 vis_draws;   // NphVisDraw[max_vsm_draws] (indirect raster only)
    u64 size;
} VsmLayout;

// The production path's virtual shadow map (vsm.c; docs/render.md
// "Shadows").
typedef struct RenderVsm {
    b32 supported;
    u32 pool_pages;
    NphGpuImage pool;     // D32_SFLOAT, NPH_VSM_POOL_ROW pages per row
    NphGpuBuffer entries; // NphVsmEntry[NPH_VSM_LEVELS * NPH_VSM_LEVEL_ENTRIES]
    NphGpuBuffer phys;    // NphVsmPhys[pool_pages]
    NphGpuBuffer work;    // VsmLayout
    NphGpuBuffer stats[NPH_GPU_FRAMES_IN_FLIGHT]; // NphVsmCounters read back
    u64 stats_frame[NPH_GPU_FRAMES_IN_FLIGHT];    // 1 + the frame copied into stats[i], 0: none
    u32 stats_bias[NPH_GPU_FRAMES_IN_FLIGHT];     // the resolution bias that frame drew with
    u32 overflow_logged;                          // NphRenderVsmOverflow bits said once
    // The resolution bias (levels coarser than resolution-matched): raised
    // by each overflow at the current bias, lowered after `roomy` frames
    // in a row that would have fitted a step finer.
    u32 lod_bias;
    u32 roomy;
    u32 frame_bias; // the bias this frame draws with (lod_bias as it began)
    VsmLayout layout;
    NphGpuShader shaders[5]; // pages, cull, raster (mesh or vertex), clear, request
    NphGpuPipeline pages;
    NphGpuPipeline cull;
    NphGpuPipeline raster;
    NphGpuPipeline clear;
    NphGpuPipeline request; // the blended draws' page requests (vsm_request.frag)
    u32 sun_slot;           // the light it shadows, NPH_U32_MAX: none
    b32 initialize;         // the page table and the pages' records still need clearing
    // The clipmap as the pages hold it.
    b32 continuous; // the previous production frame had the map (else everything is dropped)
    NphVec3 direction;
    NphDVec3 anchor;
    f64 depth_reference[NPH_VSM_LEVELS];
    u32 frames; // frames that had the map: the pages' ages
    // This frame.
    b32 frame_active;
    b32 frame_initialize;
    u64 view_address; // its NphVsmView
    struct {
        NphGpuRgBuffer work;
        NphGpuRgBuffer entries;
        NphGpuRgBuffer phys;
        NphGpuRgImage pool;
        NphGpuRgBuffer stats;
    } frame;
} RenderVsm;

// Ray-traced shadows (rt.c; docs/render.md "Shadows").
typedef struct RenderRt {
    b32 supported;      // the device has ray queries and the description allowed them
    b32 unorm_vertices; // builds read the quantised positions as they are
    NphGpuAccel tlas;
    NphGpuBuffer instances; // NphGpuAccelInstance[max_instances]: the production path's
    NphGpuBuffer scratch;   // the builds' scratch: room for the largest one
    u64 scratch_size;
    u32 *pending; // mesh slots whose bottom level waits for its build
    u32 pending_count;
    // One bottom level's geometries (a submesh each), as mesh_geometries
    // fills them for a sizing (the main thread) or a build (the build pass,
    // while the main thread waits on the graph): never both at once.
    NphGpuAccelTriangles *geometries; // NPH_RENDER_MAX_SUBMESHES
    u32 shadow_lights;                // live lights that cast shadows
    // The top level's space: the camera's shifted by tlas_offset (rt.c).
    NphDVec3 anchor;      // where its origin is in the world
    NphVec3 tlas_offset;  // the camera in it, this frame
    b32 tlas_valid;       // it holds the instances as they are (as of this frame's records)
    b32 tlas_production;  // the production path built it
    b32 frame_active;     // this frame's top level is built, for shadow rays or the radiance cache
    b32 frame_shadows;    // this frame shades with shadow rays (the *_rt pipelines)
    b32 frame_production; // its instances come from the GPU scene
    b32 frame_rebuild;    // it rebuilds the top level
    u32 frame_instances;  // instance records of this frame's top level (active ones first)
    u32 frame_builds;     // the first pending bottom levels, built this frame
    b32 unsupported_logged;
    NphGpuPipeline instance_writer; // rt_instances.comp
} RenderRt;

typedef struct RenderLight {
    u32 handle;
    NphRenderLightDesc desc;
} RenderLight;

// One draw call of the forward pass, sorted by key.
typedef struct RenderDrawCmd {
    u64 key;    // opaque: pipeline, then mesh; blended: farthest first
    u32 record; // NphRenderDrawData index in the frame buffer
    u32 mesh;   // pool slot of the mesh
    u32 first_index;
    u32 index_count;
    u8 alpha_mode;
    u8 cull;
    u8 pad[6];
} RenderDrawCmd;

// The image-based-lighting compute passes of one frame: the DFG table, the
// environment's level 0, its mip chain (up to 14 levels below level 0),
// the SH projection, and the prefiltered levels (up to 15).
#define RENDER_IBL_MAX_PASSES 40U

typedef struct RenderIblPass {
    NphGpuPipeline pipeline;
    NphRenderIblPush push;
    u32 groups[3];
} RenderIblPass;

// The per-frame upload buffer's layout (one buffer per frame in flight).
typedef struct RenderFrameLayout {
    u64 view_offset;
    u64 sky_offset;
    u64 lights_offset;
    u64 materials_offset;
    u64 draws_offset;
    u64 vis_view_offset;          // NphVisViewData (production path)
    u64 vis_updates_offset;       // NphVisInstanceUpdate[max_instance_updates]
    u64 vis_light_updates_offset; // NphVisLightUpdate[max_lights]
    u64 light_grid_offset;        // NphRenderLightGrid
    u64 rt_instances_offset;      // NphGpuAccelInstance[max_instances]: the reference's
    u64 vsm_view_offset;          // NphVsmView (production path)
    u64 gi_view_offset;           // NphRenderGiView (production path)
    u64 size;
} RenderFrameLayout;

// ---------------------------------------------------------------------------
// The production path (vis.c, vis_scene.c)
// ---------------------------------------------------------------------------

typedef enum VisShader {
    VIS_SHADER_SCATTER = 0,
    VIS_SHADER_CULL_INSTANCES,
    VIS_SHADER_CULL_MESHLETS,
    VIS_SHADER_ARGS,
    VIS_SHADER_HZB,
    VIS_SHADER_RESOLVE,
    VIS_SHADER_SW_RASTER,
    VIS_SHADER_LIGHTS,
    VIS_SHADER_LIGHT_BINS,
    VIS_SHADER_LIGHT_TILES,
    VIS_SHADER_RESOLVE_RT, // with ray-traced shadows (rt.c), where the device has ray queries
    VIS_SHADER_MESH_OPAQUE,
    VIS_SHADER_MESH_MASKED,
    VIS_SHADER_MESH_FRAG_OPAQUE,
    VIS_SHADER_MESH_FRAG_MASKED,
    VIS_SHADER_VERT_OPAQUE,
    VIS_SHADER_VERT_MASKED,
    VIS_SHADER_VERT_FRAG_OPAQUE,
    VIS_SHADER_VERT_FRAG_MASKED,
    VIS_SHADER_DEPTH,
    VIS_SHADER_DEBUG_LINE,
    VIS_SHADER_COUNT
} VisShader;

typedef enum VisCompute {
    VIS_COMPUTE_SCATTER = 0,
    VIS_COMPUTE_CULL_INSTANCES,
    VIS_COMPUTE_CULL_MESHLETS,
    VIS_COMPUTE_ARGS,
    VIS_COMPUTE_HZB,
    VIS_COMPUTE_RESOLVE,
    VIS_COMPUTE_SW_RASTER,
    VIS_COMPUTE_LIGHTS,
    VIS_COMPUTE_LIGHT_BINS,
    VIS_COMPUTE_LIGHT_TILES,
    VIS_COMPUTE_RESOLVE_RT,
    VIS_COMPUTE_RESOLVE_VSM,    // with the sun's virtual shadow map (vsm.c)
    VIS_COMPUTE_RESOLVE_RT_VSM, // with both
    // Each resolve with the radiance cache (gi.c).
    VIS_COMPUTE_RESOLVE_GI,
    VIS_COMPUTE_RESOLVE_RT_GI,
    VIS_COMPUTE_RESOLVE_VSM_GI,
    VIS_COMPUTE_RESOLVE_RT_VSM_GI,
    VIS_COMPUTE_COUNT
} VisCompute;

// The light grid's work buffer (vis light grid): the regions' offsets for
// a renderer of `lights` lights (render_vis_light_layout).
typedef struct VisLightLayout {
    u64 histogram; // u32[NPH_RENDER_ZBINS], cleared every frame
    u64 zbins;     // u32[NPH_RENDER_ZBINS]
    u64 visible;   // uvec4[lights]
    u64 staged;    // NphRenderLightData[lights]
    u64 locals;    // NphRenderLightData[lights]
    u64 globals;   // NphRenderLightData[lights]
    u64 rects;     // uvec2[lights]
    u64 size;
} VisLightLayout;

// A range to free once the frames that may still read it have completed.
typedef struct VisPendingFree {
    u64 frame; // safe from the frame with this index on
    NphRange range;
    u32 pool; // 0: the material map, 1: the index pool
} VisPendingFree;

typedef NPH_ARRAY(VisPendingFree) VisPendingFreeArray;

// One frame's render-graph handles and push-constant inputs, read by the
// pass callbacks.
typedef struct VisFrame {
    u64 view_address; // NphVisViewData
    u32 update_count;
    u32 light_update_count;
    u32 width;
    u32 height;
    NphGpuRgImage visbuffer;
    NphGpuRgImage hdr;
    NphGpuRgImage depth;        // null without blended draws
    NphGpuRgImage raster_depth; // the hardware raster's early depth test (mesh path; else null)
    NphGpuRgImage hzb;
    NphGpuRgBuffer hzb_dirty;
    NphGpuRgBuffer counters;
    NphGpuRgBuffer args;
    NphGpuRgBuffer draws;
    NphGpuRgBuffer index_pool;
    NphGpuRgBuffer stats;
    NphGpuRgBuffer lights;
    NphGpuRgBuffer light_grid;
    NphGpuRgBuffer light_tiles;
    NphGpuRgBuffer tlas; // the frame's top level, when it traces shadow rays
    // The GPU scene's tables in the graph, for the temporal resolve's motion.
    NphGpuRgBuffer instances;
    NphGpuRgBuffer prev_transforms;
    NphGpuRgBuffer clusters;
} VisFrame;

typedef struct RenderVis {
    b32 supported;           // the device can run the production path and it was not disabled
    b32 mesh_shaders;        // rasterise with mesh shaders (else indirect draws)
    NphGpuFormat hdr_format; // RENDER_HDR_COMPACT where it can, else RENDER_HDR_FORMAT
    // Blended forward pipelines into hdr_format (null when it is
    // RENDER_HDR_FORMAT): [ray-traced shadows][the virtual shadow map][cull].
    NphGpuPipeline blend_pipelines[2][2][RENDER_CULL_COUNT];
    NphGpuShader shaders[VIS_SHADER_COUNT];
    NphGpuPipeline compute[VIS_COMPUTE_COUNT];
    NphGpuPipeline raster[2]; // [opaque, masked]: mesh or vertex pipelines
    NphGpuPipeline depth;     // the visibility buffer's depth into a depth attachment

    // The GPU scene: persistent tables indexed by pool slot.
    NphGpuBuffer instances;       // NphVisInstance[max_instances]
    NphGpuBuffer prev_transforms; // NphVisTransform[max_instances]
    NphGpuBuffer meshes;          // NphVisMesh[max_meshes]
    NphGpuBuffer material_map;    // u32[max_material_slots]: material handle values
    NphGpuBuffer index_pool;      // u16[meshlet_index_pool] (indirect raster only)
    NphRangeAllocator material_ranges;
    NphRangeAllocator index_ranges;
    VisPendingFreeArray pending;

    // The culling lists: persistent so that their addresses are known when
    // the view is written, imported into the graph every frame.
    NphGpuBuffer counters;      // NphVisCounters
    NphGpuBuffer args;          // NphVisArgs
    NphGpuBuffer cull_meshlets; // uvec2[max_cull_meshlets]: phase 0's meshlet candidates
    NphGpuBuffer deferred;      // uvec2[max_deferred] meshlets, then u32[max_deferred] instances
    NphGpuBuffer clusters;      // NphVisCluster[max_visible_clusters]
    NphGpuBuffer lists;         // u32[2][NPH_VIS_RASTER_KINDS][max_visible_clusters]
    NphGpuBuffer draws;         // NphVisDraw[2][2][max_visible_clusters] (indirect raster only)
    NphGpuBuffer stats[NPH_GPU_FRAMES_IN_FLIGHT]; // NphVisCounters read back
    u64 stats_frame[NPH_GPU_FRAMES_IN_FLIGHT];    // frame index + 1 each holds (0: none)

    // The lights: the persistent table, the grid's work buffer, and the
    // tile masks (sized by the target, with the HZB).
    NphGpuBuffer lights;     // NphVisLight[max_lights]
    NphGpuBuffer light_grid; // VisLightLayout
    NphGpuBuffer light_tiles;
    VisLightLayout light_layout;
    u32 light_tiles_x;
    u32 light_tiles_y;
    u32 light_words; // mask words: one per 32 lights, each over every tile
    // Lights whose records must reach the GPU: a flag per slot and a list.
    u8 *light_flags;
    u32 *light_dirty;
    u32 light_dirty_count;

    // Instances whose records must reach the GPU, and those sent last frame.
    u8 *slot_state; // RenderSlotState bits per instance slot
    u32 *dirty;
    u32 dirty_count;
    u32 *sent;
    u32 sent_count;
    u32 *sent_next;
    u32 sent_next_count;
    u32 frame_changed; // instances whose records this frame sends (not catch-ups)
    // Instances with a blended material: the CPU draws those itself.
    u32 *blend;
    u32 blend_count;
    b32 blend_stale; // a material's alpha mode changed: rebuild the list

    // The hierarchical depth of the last production frame.
    NphGpuImage hzb;
    u32 hzb_width; // its mip 0
    u32 hzb_height;
    u32 hzb_mips;
    u32 target_width; // the target it was built for
    u32 target_height;
    b32 hzb_valid;
    // The tiles phase 1 drew into, a bit each: the pyramid after phase 1
    // rebuilds those alone (vis_hzb.comp mode 2).
    NphGpuBuffer hzb_dirty;
    u32 hzb_tiles_x;
    u32 hzb_tiles_y;
    b32 hzb_full_rebuild; // tests: rebuild the whole pyramid after phase 1 too
    // That frame's view, for phase 0's tests against its HZB (and its
    // projection without the jitter, for the temporal resolve's motion).
    NphMat4 prev_view;
    NphMat4 prev_view_proj;
    NphVec4 prev_projection;
    NphDVec3 prev_camera;
    NphVec2 prev_jitter; // UV
    // 1 + the device frame index of that frame (0: none, or another extent
    // since): the motion blur measures motion only from the frame just before.
    u64 prev_frame;

    NphRenderFrameStats stats_last; // the vis_* fields of the last read-back
    b32 fallback_logged;            // PRODUCTION requested without the path: said once
    u32 overflow_logged;            // overflow bits already reported
    b32 updates_logged;             // instance updates waited for a frame: said once
    VisFrame frame;
} RenderVis;

// The jitter sequence's length: eight Halton (2, 3) points, which the
// history's 16-frame memory covers twice (Karis 2014 uses eight).
#define RENDER_TAA_PHASES 8U

// The production path's temporal anti-aliasing (taa.c): the history of
// resolved frames, two images written in turn.
typedef struct RenderTaa {
    NphGpuShader shader;         // taa.frag
    NphGpuShader history_shader; // taa_history.frag
    // taa_history.frag into the history alone (RenderVis.hdr_format): the
    // motion blur reads it and writes the target (motion_blur.c).
    NphGpuPipeline history_only;
    NphGpuSampler sampler; // bilinear, clamped to the edge: the history's taps
    NphGpuImage history[2];
    NphGpuFormat format; // the history's: RenderVis.hdr_format
    u32 width;
    u32 height;
    u32 write;              // the history this frame writes; the other holds the last one
    b32 valid;              // history[write ^ 1] holds the last frame resolved temporally
    u64 sequence;           // frames resolved temporally so far: the jitter's phase
    u64 last_frame;         // 1 + the device frame index of the last frame resolved temporally
    f32 exposure;           // the exposure the history was resolved with
    b32 frame_active;       // this frame resolves temporally
    b32 frame_history;      // and blends with the history
    b32 frame_history_only; // and writes only the history (the motion blur writes the target)
    NphVec2 frame_jitter;   // this frame's jitter, pixels
    NphRenderTaaPush push;  // this frame's, without the transient images' indices
} RenderTaa;

// One pass of the bloom chain or the metering (post.c): its callback's data.
typedef struct RenderPostPass {
    NphRenderer *renderer;
    u32 mode; // NPH_RENDER_POST_*
    u32 mip;  // the chain's mip it writes
} RenderPostPass;

// Bloom and the exposure (post.c).
typedef struct RenderPost {
    NphGpuShader bloom_shader;
    NphGpuShader exposure_shader;
    // bloom.comp by kernel (NPH_RENDER_POST_DOWN_FIRST to _UP), specialised
    // for the chain's format.
    NphGpuPipeline bloom[NPH_RENDER_POST_UP + 1];
    NphGpuPipeline exposure; // exposure.comp
    NphGpuFormat format;     // the chain's: B10G11R11 where it can be stored and filtered
    NphGpuBuffer metering;   // NphRenderExposureData
    NphGpuBuffer readback[NPH_GPU_FRAMES_IN_FLIGHT];
    u64 readback_frame[NPH_GPU_FRAMES_IN_FLIGHT];    // 1 + the frame readback[i] meters; 0: none
    f32 readback_exposure[NPH_GPU_FRAMES_IN_FLIGHT]; // the exposure that frame was drawn with
    f32 ev100;                                       // NPH_RENDER_EXPOSURE_AUTO: the adapted EV100
    b32 adapted;       // ev100 holds a value (else the view's starts it)
    f32 metered_ev100; // the latest metering's target
    b32 metered;       // metered_ev100 holds one
    // This frame's.
    b32 frame_bloom;
    b32 frame_meter;
    f32 frame_weight; // the bloom's weight in the image
    f32 frame_ev100;
    u32 frame_levels; // the chain's mips (0: no chain)
    NphGpuRgImage frame_chain;
    RenderPostPass passes[2U * NPH_RENDER_BLOOM_LEVELS + 1U];
} RenderPost;

// Depth of field on both pipelines (dof.c): every image is transient, made
// each frame that has it.
typedef struct RenderDof {
    NphGpuShader shaders[4]; // dof_setup, dof_tiles, dof_gather, dof_resolve
    NphGpuPipeline setup[2]; // [the reference's depth image, the visibility buffer]
    NphGpuPipeline tiles;
    NphGpuPipeline gather;
    NphGpuPipeline resolve[2];      // [plain, B10G11R11 dithered]
    NphGpuFormat full_depth_format; // the full-resolution depths: R16F where storable, else R32F
    // This frame.
    b32 frame_active;
    f32 frame_bias; // the circle's radius in pixels at reverse-Z depth d: bias + scale * d
    f32 frame_scale;
    u32 frame_width;
    u32 frame_height;
    u32 frame_tiles_x;
    u32 frame_tiles_y;
    b32 frame_visbuffer;           // the depth is the visibility buffer's (the production path)
    NphGpuFormat frame_hdr_format; // the output's too
    NphGpuRgImage frame_hdr;       // as drawn
    NphGpuRgImage frame_depth;
    NphGpuRgImage frame_half;
    NphGpuRgImage frame_full_depth;
    NphGpuRgImage frame_tiles;
    NphGpuRgImage frame_neighbours;
    NphGpuRgImage frame_far;
    NphGpuRgImage frame_near;
    NphGpuRgImage frame_out; // the HDR image with depth of field
} RenderDof;

// The production path's ambient occlusion (ao.c): its images are transient,
// made each frame that has it.
typedef struct RenderAo {
    NphGpuShader depth_shader;   // ao_depth.comp
    NphGpuShader ao_shader;      // ao.comp
    NphGpuShader denoise_shader; // ao_denoise.comp
    NphGpuPipeline depth;
    NphGpuPipeline ao;
    NphGpuPipeline denoise;
    // This frame, at half resolution.
    b32 frame_active;
    NphRenderAoPush frame_push; // all but the images' indices
    NphGpuRgImage frame_depths; // reverse-Z depths, NPH_RENDER_AO_MIPS mips
    NphGpuRgImage frame_raw;    // each texel's slice's occlusion and the distance
    NphGpuRgImage frame_out;    // the denoised occlusion and the depth
} RenderAo;

// The production path's radiance cache (gi.c): its slots persist across
// frames; the frame's fields are set by render_gi_frame_begin and
// render_gi_declare.
typedef struct RenderGi {
    b32 supported;               // render_gi_device_supported, set with the production path
    b32 cleared;                 // its slots have been zeroed (the first frame that has it)
    u32 capacity;                // slots (NphRendererDesc.gi_cells)
    u32 sun_slot;                // the light whose bounce the update traces (NPH_U32_MAX: none)
    NphGpuBuffer keys;           // u32 fingerprints
    NphGpuBuffer sums;           // uvec4: this frame's samples
    NphGpuBuffer cells;          // uvec4: irradiance, count, age
    NphGpuShader update_shader;  // gi_update.comp
    NphGpuShader resolve_shader; // gi_resolve.comp
    NphGpuPipeline update;
    NphGpuPipeline resolve;
    b32 frame_active;
    u32 frame_rays_x; // the update's samples across and down the target
    u32 frame_rays_y;
    NphGpuRgImage frame_visbuffer;
    NphGpuRgBuffer frame_keys;
    NphGpuRgBuffer frame_sums;
    NphGpuRgBuffer frame_cells;
} RenderGi;

// What the radiance cache's update reads of the frame (vis.c).
typedef struct RenderGiInputs {
    NphGpuRgImage visbuffer;
    NphGpuRgBuffer clusters;
    NphGpuRgBuffer instances;
    NphGpuRgBuffer meshes;
    NphGpuRgBuffer material_map;
    NphGpuRgBuffer tlas;
    NphGpuRgImage env;
    NphGpuRgBuffer sh;
} RenderGiInputs;

// The production path's motion blur (motion_blur.c): the motion image and
// the tiles are transient, made each frame that blurs.
typedef struct RenderMotion {
    NphGpuShader motion_shader; // motion.comp
    NphGpuShader tiles_shader;  // motion_tiles.comp
    NphGpuShader blur_shader;   // motion_blur.frag
    NphGpuPipeline motion;
    NphGpuPipeline tiles;
    // This frame.
    b32 frame_active;
    f32 frame_scale; // a pixel of motion's blur half-extent
    u32 frame_tiles_x;
    u32 frame_tiles_y;
    b32 frame_glare;           // the gather mixes the bloom in (no temporal resolve did)
    NphGpuRgImage frame_color; // what it blurs: the temporal history, or the HDR image
    NphGpuRgImage frame_motion;
    NphGpuRgImage frame_tiles;
    NphGpuRgImage frame_neighbours;
} RenderMotion;

struct NphRenderer {
    NphAllocator *alloc;
    NphGpuDevice *dev;
    NphRendererDesc desc;
    char name[RENDER_NAME_MAX];
    char shader_dir[512];

    // Resource pools: a handle's index is the slot in the parallel array.
    NphHandlePool texture_pool;
    NphHandlePool mesh_pool;
    NphHandlePool material_pool;
    NphHandlePool instance_pool;
    NphHandlePool light_pool;
    RenderTexture *textures;
    RenderMesh *meshes;
    RenderMaterial *materials;
    RenderInstance *instances;
    RenderLight *lights;

    // Samplers: material samplers by [wrap_u][wrap_v][nearest], and the
    // trilinear clamp sampler of the lighting tables.
    NphGpuSampler material_samplers[3][3][2];
    NphGpuSampler clamp_sampler;

    NphGpuShader shaders[RENDER_SHADER_COUNT];
    NphGpuPipeline forward[3][RENDER_CULL_COUNT];    // [alpha mode][cull]
    NphGpuPipeline forward_rt[3][RENDER_CULL_COUNT]; // with ray-traced shadows (rt.c)
    NphGpuPipeline sky;
    NphGpuPipeline compute[RENDER_COMPUTE_COUNT];
    RenderTargetPipelines target_pipelines[RENDER_TARGET_FORMATS];

    // Per frame in flight: records for the GPU (view, sky, lights,
    // materials, draws), persistently mapped.
    NphGpuBuffer frame_buffers[NPH_GPU_FRAMES_IN_FLIGHT];
    u8 *frame_maps[NPH_GPU_FRAMES_IN_FLIGHT];
    u64 frame_addresses[NPH_GPU_FRAMES_IN_FLIGHT];
    RenderFrameLayout layout;
    u64 material_version;                            // bumped by every material change
    u64 material_uploaded[NPH_GPU_FRAMES_IN_FLIGHT]; // the version each slot holds (0: none)
    RenderDrawCmd *draw_cmds;                        // max_draws, rebuilt every frame
    u32 hidden_count;                                // live instances with `hidden` set
    u32 opaque_count;     // draw_cmds[0, opaque_count): opaque and masked
    u32 blend_count;      // then the blended ones
    u64 last_frame_index; // nph_render_frame's last frame (NPH_U64_MAX: none)
    b32 pending_uploads;

    // Image-based lighting.
    NphGpuImage env_cube;      // RENDER_IBL_FORMAT, environment_size, full chain
    NphGpuImage specular_cube; // RENDER_IBL_FORMAT, specular_size, specular_mips
    NphGpuImage brdf_lut;      // RENDER_IBL_FORMAT, brdf_lut_size
    NphGpuBuffer sh_buffer;    // NphVec4[9]
    u32 env_mips;
    b32 lut_ready;
    b32 env_dirty;
    NphRenderEnvironmentDesc env;
    RenderIblPass ibl_passes[RENDER_IBL_MAX_PASSES]; // this frame's, read by the callbacks
    u32 ibl_pass_count;

    // Debug lines.
    NphGpuBuffer debug_buffers[RENDER_DEBUG_BUFFERS];
    NphRenderDebugVertex *debug_maps[RENDER_DEBUG_BUFFERS];
    u64 debug_addresses[RENDER_DEBUG_BUFFERS];
    u32 debug_write;             // the buffer being filled
    NphAtomicU32 debug_count[2]; // lines reserved: [0] depth tested, [1] overlay
    u32 debug_draw_counts[2];    // lines the frame being recorded draws
    u32 debug_draw_buffer;       // and from which buffer
    NphDVec3 debug_origin;
    NphDVec3 debug_frame_origin; // the origin of the lines being drawn
    u64 debug_flip_frame;        // nph_gpu_frame_index + 1 of the last flip; 0: none

    // The production path.
    RenderVis vis;

    // Ray-traced shadows.
    RenderRt rt;

    // The production path's virtual shadow map.
    RenderVsm vsm;

    // The production path's temporal anti-aliasing.
    RenderTaa taa;

    // Bloom and the exposure.
    RenderPost post;

    // The production path's motion blur.
    RenderMotion motion;

    // Depth of field.
    RenderDof dof;

    // The production path's ambient occlusion.
    RenderAo ao;

    // The production path's radiance cache.
    RenderGi gi;

    // The frame being recorded (read by the pass callbacks).
    NphRenderPush push;
    NphRenderTonemapPush tonemap_push;
    const RenderTargetPipelines *frame_target;
    NphGpuRgImage hdr_rg; // the frame's HDR target, resolved in the tonemap pass
    u32 frame_slot;
    b32 frame_production; // the frame is drawn by the production path
};

// The record at `offset` in the upload buffer of the frame being recorded,
// as a void pointer so that it converts to the record's type.
NPH_INLINE void *render_frame_at(const NphRenderer *r, u64 offset)
{
    return r->frame_maps[r->frame_slot] + offset;
}

// renderer.c
NphResult render_load_shader(NphRenderer *r, RenderShader shader, const char *file);
// The forward pipeline of an alpha mode and cull variant drawing into
// `format` (the reference's HDR target, or the production path's).
// With `ray_query`, the variant that traces shadow rays (rt.c); with
// `vsm`, the one that reads the sun's virtual shadow map (vsm.c).
NphResult render_forward_pipeline(NphRenderer *r, u32 alpha, u32 cull, NphGpuFormat format,
                                  bool ray_query, bool vsm, NphGpuPipeline *out);
NphResult render_load_shader_file(NphRenderer *r, const char *file, NphGpuShader *out);
const RenderTexture *render_texture_get(const NphRenderer *r, NphRenderTexture texture);
u32 render_material_sampler(const NphRenderer *r, const NphRenderTextureBinding *binding);
// The pool slot of a live handle, or NPH_U32_MAX for null and stale ones.
u32 render_slot_of(const NphHandlePool *pool, u32 value);

// frame.c
// `ray_tracing`: room for the reference's top-level instance records.
void render_frame_layout(const NphRendererDesc *desc, bool ray_tracing, RenderFrameLayout *out);
// A light's shading record with its position given (camera relative for
// the reference; the high floats of its split for the GPU table).
void render_light_data(const NphRenderLightDesc *desc, NphVec3 position, NphRenderLightData *out);

// ibl.c
NphResult render_ibl_create(NphRenderer *r);
void render_ibl_destroy(NphRenderer *r);
// Declares the DFG table pass (first frame) and the environment passes
// (when the environment changed). `out_updated` tells whether any ran.
NphResult render_ibl_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage *out_env,
                             NphGpuRgImage *out_specular, NphGpuRgImage *out_lut,
                             NphGpuRgBuffer *out_sh, b32 *out_updated);

// debug_draw.c
NphResult render_debug_create(NphRenderer *r);
void render_debug_destroy(NphRenderer *r);
// Freezes the lines added so far for this frame and switches writers to
// the next buffer.
void render_debug_begin_frame(NphRenderer *r);

// rt.c: ray-traced shadows' acceleration structures.
NphResult render_rt_create(NphRenderer *r);
void render_rt_destroy(NphRenderer *r);
// A mesh's bottom level: created with the mesh, built in the next frame
// that traces.
NphResult render_rt_mesh_create(NphRenderer *r, u32 slot, const NphRenderMeshDesc *desc);
void render_rt_mesh_destroy(NphRenderer *r, u32 slot);
// A light's shadows were turned on (+1) or off (-1).
void render_rt_shadow_lights(NphRenderer *r, i32 change);
// Whether this frame builds the top level: to trace shadow rays (some
// light casts shadows that the virtual shadow map does not take, `handled`
// of them: RenderRt.frame_shadows), or for the radiance cache's rays (`gi`),
// and the renderer can (logged once when shadows cannot). Sets
// RenderRt.frame_* and the top level's space for this frame's camera.
bool render_rt_frame_begin(NphRenderer *r, bool production, NphDVec3 camera, u32 handled, bool gi);
// The reference's instance records, written by the CPU into the frame's
// upload buffer, in the top level's space.
void render_rt_write_instances(NphRenderer *r);
// Declares this frame's passes: on the production path the instance
// records' writer, which reads the GPU scene's instance and mesh tables
// (`instance_table`, `mesh_table`, and the vis `counters`, whose
// rt_instances it appends to; null handles on the reference, whose records
// render_rt_write_instances wrote), then the builds (the bottom levels
// pending now, then the top level). Returns the top level's buffer in the
// graph for the passes that trace (NPH_GPU_RG_USE_ACCEL_READ).
NphResult render_rt_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgBuffer instance_table,
                            NphGpuRgBuffer mesh_table, NphGpuRgBuffer counters,
                            NphGpuRgBuffer *out_tlas);

// vsm.c: the production path's virtual shadow map.
// Whether the renderer can have it (logged reasons are vis.c's).
bool render_vsm_device_supported(const NphRenderer *r);
NphResult render_vsm_create(NphRenderer *r);
void render_vsm_destroy(NphRenderer *r);
// Light `slot` was created, changed, or destroyed (its record as it now
// is): the sun is the live directional light that casts shadows in the
// lowest slot; when it changes, the records of the old and the new sun are
// sent again. Constant time but where the sun itself stops being one.
void render_vsm_light_changed(NphRenderer *r, u32 slot);
// Whether this production frame has the map, and if so its NphVsmView,
// written into the frame's records (RenderVsm.view_address).
bool render_vsm_frame_begin(NphRenderer *r, const NphRenderView *view, const NphMat4 *proj,
                            const NphMat4 *inv_view_proj, u32 height, bool production);
// The statistics of the frame that last completed in this slot, into
// RenderVis.stats_last.
void render_vsm_read_stats(NphRenderer *r);
// `vsm clear`, then `vsm invalidate` (before the scatter, which replaces the
// instance records it reads).
NphResult render_vsm_declare_begin(NphRenderer *r, NphGpuRenderGraph *graph,
                                   NphGpuRgBuffer instances, NphGpuRgBuffer meshes);
// A pass that requests pages (the depth pyramid's mip 0) writes the work.
void render_vsm_request_pages(const NphRenderer *r, NphGpuRgPass *pass);
// The page requests of the blended surfaces: `draw` records the blended
// draws with the pipeline it is given, against `depth` (the opaque
// surfaces'), before render_vsm_declare_pages.
NphResult render_vsm_declare_blend_requests(NphRenderer *r, NphGpuRenderGraph *graph,
                                            NphGpuRgImage depth, NphGpuRgExecuteFn draw);
// The page tables, the allocation, the casters' culling, the page raster,
// and the statistics, after the requests.
NphResult render_vsm_declare_pages(NphRenderer *r, NphGpuRenderGraph *graph,
                                   NphGpuRgBuffer instances, NphGpuRgBuffer meshes);
// What a pass that samples the map reads.
void render_vsm_read(const NphRenderer *r, NphGpuRgPass *pass);

// post.c: bloom and the exposure.
NphResult render_post_create(NphRenderer *r);
void render_post_destroy(NphRenderer *r);
// The frame's EV100: the view's, or for NPH_RENDER_EXPOSURE_AUTO the
// adapted one, moved toward the latest metering read back. Sets
// RenderPost.frame_*.
f32 render_post_frame_begin(NphRenderer *r, const NphRenderView *view);
// The bloom chain and the metering over the frame's HDR image (r->hdr_rg),
// once it is complete, before the final pass.
NphResult render_post_declare(NphRenderer *r, NphGpuRenderGraph *graph, u32 width, u32 height);
// What the final pass (the tonemap, or the temporal resolve) reads of it.
void render_post_read(const NphRenderer *r, NphGpuRgPass *pass);
// What the final pass reads of the bloom (NphRenderTonemapPush's fields).
typedef struct RenderPostComposite {
    u32 bloom_index;   // bindless sampled: the chain
    f32 bloom;         // the bloom's weight in the image (0: none)
    f32 bloom_scale;   // 1 / the chain's levels
    u32 sampler_index; // bilinear, clamped to the edge
    NphVec2 bloom_uv;  // a pixel's step in mip 0's UV
} RenderPostComposite;
// The final pass's bloom, inside its callback.
RenderPostComposite render_post_composite(const NphRenderer *r, NphGpuRenderGraph *graph);
// The histogram's mean log2 between its percentiles, as exposure.comp
// computes it, and its pixel count.
void render_exposure_average(const u32 *histogram, f32 *out_log2, u32 *out_count);
// The EV100 an averaging meter gives a mean log2 pre-exposed luminance
// measured at `exposure`, with a compensation in stops (positive: brighter).
f32 render_exposure_target_ev100(f32 average_log2, f32 exposure, f32 compensation);

// taa.c: the production path's temporal anti-aliasing.
// After render_vis_create: the history-only pipeline is made in its HDR format.
NphResult render_taa_create(NphRenderer *r);
void render_taa_destroy(NphRenderer *r);
// The temporal resolve's pipeline into a target of `format`.
NphResult render_taa_target_pipeline(NphRenderer *r, RenderTargetPipelines *t, NphGpuFormat format);
// Whether this frame resolves temporally: a lit production view with
// NPH_RENDER_AA_TEMPORAL. If so, keeps the history images the size of the
// target, decides whether the history continues (RenderTaa.frame_history),
// and returns the frame's jitter in pixels in `out_jitter` (zero otherwise).
NphResult render_taa_frame_begin(NphRenderer *r, const NphRenderView *view, u32 width, u32 height,
                                 bool production, NphVec2 *out_jitter);
// The temporal resolve into `out` (the caller's target) and the next
// history, from the frame's HDR image and visibility buffer; after
// render_vis_frame and the transparent pass. With `out` null it writes the
// history alone, for the motion blur to read and tonemap. `exposure` is the
// frame's, which the HDR image is pre-exposed with. `out_history` receives
// the history it writes.
NphResult render_taa_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage out,
                             f32 exposure, NphGpuRgImage *out_history);
// The jitter of the `sequence`th frame resolved temporally, in pixels: a
// Halton (2, 3) sequence of RENDER_TAA_PHASES points in [-0.5, 0.5).
NphVec2 render_taa_jitter(u64 sequence);
// The reconstruction filter's weights of the 3 x 3 samples around a pixel,
// row by row from the top left, for the frame's jitter (pixels): the
// sample at offset o lies at o - jitter from the pixel's centre. They sum
// to 1 (taa_common.glsl taa_filter_weight is the shader's twin).
void render_taa_weights(NphVec2 jitter, f32 out[9]);

// dof.c: depth of field.
NphResult render_dof_create(NphRenderer *r);
void render_dof_destroy(NphRenderer *r);
// Whether this frame has depth of field: a lit view with an aperture. If so
// sets RenderDof.frame_* for a target `height` pixels tall.
bool render_dof_frame_begin(NphRenderer *r, const NphRenderView *view, u32 height);
// The setup, the neighbourhoods, the gather, and the resolve, which make
// `out`, the HDR image with depth of field (of `hdr_format`, sampled), from
// `hdr` (sampled) and `depth` (the visibility buffer when `visbuffer`, else
// a sampled depth image); after the scene, before render_post_declare,
// whose passes and the frame's later ones then read `out` as the HDR image.
NphResult render_dof_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage hdr,
                             NphGpuFormat hdr_format, NphGpuRgImage depth, bool visbuffer,
                             u32 width, u32 height, NphGpuRgImage *out);

// ao.c: the production path's ambient occlusion.
NphResult render_ao_create(NphRenderer *r);
void render_ao_destroy(NphRenderer *r);
// Whether this frame has ambient occlusion: a production view with a radius
// whose image shows the environment's light where there is any (lit, or the
// indirect debug view) or shows the occlusion (its debug view). If so sets
// RenderAo.frame_* from `proj`, the frame's projection (jitter included),
// for a `width` x `height` target.
bool render_ao_frame_begin(NphRenderer *r, const NphRenderView *view, const NphMat4 *proj,
                           u32 width, u32 height, bool production);
// The depths (RenderAo.frame_depths), before the depth pyramid's passes,
// which write their mip 0 (vis_hzb.comp).
NphResult render_ao_declare_depths(NphRenderer *r, NphGpuRenderGraph *graph);
// Once the pyramid's passes have: the depths' mips, the occlusion, and its
// denoise into RenderAo.frame_out, which the resolve upsamples.
NphResult render_ao_declare(NphRenderer *r, NphGpuRenderGraph *graph);
// RenderAo.frame_out's sampled bindless index (a pass's callback).
u32 render_ao_index(const NphRenderer *r, NphGpuRenderGraph *graph);

// gi.c: the production path's radiance cache.
// Whether the renderer makes it: not disabled, and the device has ray
// queries (decided when the production path is created, whose resolve
// pipelines are specialised on it).
bool render_gi_device_supported(const NphRenderer *r);
NphResult render_gi_create(NphRenderer *r);
void render_gi_destroy(NphRenderer *r);
// Light `slot` was created, changed, or destroyed: the sun whose light the
// cache's rays find reflected is the live directional light in the lowest
// slot. Constant time but where the sun itself stops being one.
void render_gi_light_changed(NphRenderer *r, u32 slot);
// Whether a frame of `view` takes the cache: a production view asking for
// it whose image shows the diffuse indirect light (lit, or the indirect and
// irradiance debug views), on a renderer that has it. The top level must be
// built for it (render_rt_frame_begin).
bool render_gi_frame_wanted(const NphRenderer *r, const NphRenderView *view, bool production);
// Whether this frame has it; if so writes its NphRenderGiView into the
// frame's records (render_gi_view_address) for a `width` x `height` target,
// after the ambient occlusion's and the temporal resolve's frame begins.
bool render_gi_frame_begin(NphRenderer *r, const NphRenderView *view, u32 width, u32 height,
                           bool production);
u64 render_gi_view_address(const NphRenderer *r);
// Declares the update and the resolve (and, the first time, the slots'
// clear) once the frame's top level and visibility buffer are declared.
NphResult render_gi_declare(NphRenderer *r, NphGpuRenderGraph *graph, const RenderGiInputs *in);
// Declares `pass`'s reads of the cells (the resolve's shading), if the frame
// has them.
void render_gi_read(const NphRenderer *r, NphGpuRgPass *pass);
// The keys on the CPU, gi_common.glsl's twins: a camera's level table for
// cells 1 / (2 scale) to 1 / scale of their distance across, a point's
// level and key, and the mix that makes a key's bucket and fingerprint.
void render_gi_levels(NphDVec3 camera, f64 scale, NphRenderGiLevel *levels);
i32 render_gi_level(f32 dist);
u64 render_gi_key(const NphRenderGiLevel *levels, NphVec3 p, NphVec3 n);
u64 render_gi_mix(u64 k);
// The update's block: the smallest square of pixels that keeps a
// `width` x `height` frame's samples within `rays`.
u32 render_gi_stride(u32 width, u32 height, u32 rays);

// motion_blur.c: the production path's motion blur.
NphResult render_motion_create(NphRenderer *r);
void render_motion_destroy(NphRenderer *r);
// The gather's pipeline into a target of `format`.
NphResult render_motion_target_pipeline(NphRenderer *r, RenderTargetPipelines *t,
                                        NphGpuFormat format);
// Whether this frame blurs: a lit production view with a shutter that
// continues the production frame just before it, in the same extent
// (`width` x `height`), without a cut. Sets RenderMotion.frame_*; before
// render_vis_frame.
bool render_motion_frame_begin(NphRenderer *r, const NphRenderView *view, u32 width, u32 height,
                               bool production);
// The motion, the tiles, and the gather fused with the tonemap into `out`
// (the caller's target), blurring `color`: the history the temporal resolve
// wrote this frame, or the HDR image, whose bloom the gather then mixes in
// (`glare`). After render_post_declare.
NphResult render_motion_declare(NphRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage out,
                                NphGpuRgImage color, bool glare);

// vis.c: the production path's pipelines and frames.
// Logs a pipeline's compiled statistics (registers, spills; render_perf
// reads them), where the device reports them.
void render_log_statistics(NphRenderer *r, NphGpuPipeline pipeline, const char *name);
// Whether the device can run the production path (logged reasons).
bool render_vis_device_supported(const NphRenderer *r);
NphResult render_vis_create(NphRenderer *r);
void render_vis_destroy(NphRenderer *r);
// Creates the production path's pipelines for a target format.
NphResult render_vis_target_pipelines(NphRenderer *r, RenderTargetPipelines *t,
                                      NphGpuFormat format);
// The production path's frame: the view record, the instance updates,
// and every pass from the scene update to the resolve and the transparent
// surfaces, ending in the HDR image `*out_hdr` that the tonemap reads.
// `view_proj`, `view_m`, and `proj` are the frame's camera-relative
// matrices.
// `blend_requests`, where the frame has blended draws, records them with
// the pipeline RenderVsm.request (the sun's virtual shadow map asks for
// their pages with it; it is not called without the map).
NphResult render_vis_frame(NphRenderer *r, const NphRenderView *view, NphGpuRenderGraph *graph,
                           u32 width, u32 height, const NphMat4 *view_m, const NphMat4 *proj,
                           NphGpuRgImage env, NphGpuRgImage specular, NphGpuRgImage lut,
                           NphGpuRgBuffer sh, NphGpuRgExecuteFn blend_requests,
                           NphGpuRgImage *out_hdr, NphRenderFrameStats *stats);
// The bindless storage index of the frame's visibility buffer, for the
// tonemap pass's depth-tested debug lines (valid inside callbacks).
u32 render_vis_visbuffer_index(const NphRenderer *r, NphGpuRenderGraph *graph);
// The transparent surfaces of a production frame, after render_vis_frame:
// the visibility buffer's depth into a depth attachment (unless the frame
// already has it), then `draw` (the forward pipelines' blended draws) over
// the HDR image.
NphResult render_vis_declare_transparent(NphRenderer *r, NphGpuRenderGraph *graph,
                                         NphGpuRgExecuteFn draw, NphGpuRgImage env,
                                         NphGpuRgImage specular, NphGpuRgImage lut,
                                         NphGpuRgBuffer sh);

// vis_scene.c: the GPU scene.
NphResult render_vis_scene_create(NphRenderer *r);
void render_vis_scene_destroy(NphRenderer *r);
// Uploads a mesh's meshlet streams and writes its table record.
NphResult render_vis_mesh_create(NphRenderer *r, u32 slot, const NphRenderMeshDesc *desc);
void render_vis_mesh_destroy(NphRenderer *r, u32 slot);
// Instance changes: a created instance gets its material entries and a
// fresh record; any change marks it to be sent; a destroyed one is sent
// as not visible and its entries freed.
NphResult render_vis_instance_created(NphRenderer *r, u32 slot);
void render_vis_instance_changed(NphRenderer *r, u32 slot);
void render_vis_instance_destroyed(NphRenderer *r, u32 slot);
// A material's alpha mode may have changed: the blend list is rebuilt.
void render_vis_materials_changed(NphRenderer *r);
// A light was created, changed, or destroyed: its record is sent with the
// next production frame.
void render_vis_light_changed(NphRenderer *r, u32 slot);
// Writes the changed lights' records into the frame's upload buffer (all
// of them: the list holds max_lights) and returns how many.
u32 render_vis_write_light_updates(NphRenderer *r);
// The light grid work buffer's regions for `lights` lights.
void render_vis_light_layout(u32 lights, VisLightLayout *out);
// Writes this frame's instance updates into the frame's upload buffer
// (at most max_instance_updates; the rest wait) and returns how many.
u32 render_vis_write_updates(NphRenderer *r);
// Frees what frames up to `completed` can no longer read.
void render_vis_release_pending(NphRenderer *r, u64 current_frame);
// The instances the CPU draws itself (blended materials).
const u32 *render_vis_blend_list(NphRenderer *r, u32 *out_count);

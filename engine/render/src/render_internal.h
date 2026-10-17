// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal state of the renderer (renderer.c, frame.c, ibl.c,
// debug_draw.c, and the production path in vis.c and vis_scene.c). See
// render/renderer.h for the contract and docs/render.md for the design.

#include <nepheline/core/array.h>
#include <nepheline/core/atomic.h>
#include <nepheline/core/handle.h>
#include <nepheline/core/range_alloc.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/render/meshlet.h>
#include <nepheline/render/renderer.h>

#include <nph_render.h>
#include <nph_vis.h>

#define RENDER_NAME_MAX 64U

// The formats of the renderer's own images.
#define RENDER_HDR_FORMAT   NPH_GPU_FORMAT_R16G16B16A16_SFLOAT
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
    // The production path (vis_scene.c): the meshlet streams and the
    // mesh's corners in the index pool (indirect raster only).
    NphGpuBuffer meshlets;
    NphGpuBuffer meshlet_bounds;
    NphGpuBuffer meshlet_vertices;
    NphGpuBuffer meshlet_triangles;
    NphGpuBuffer submesh_slots;
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
    VIS_SHADER_TILE_BINS,
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
    VIS_COMPUTE_TILE_BINS,
    VIS_COMPUTE_RESOLVE_SKY, // vis_resolve.comp specialised for NPH_VIS_BIN_SKY
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
    NphGpuRgImage depth; // null without blended draws
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
    NphGpuRgBuffer tile_class;
    NphGpuRgBuffer tile_lists;
} VisFrame;

typedef struct RenderVis {
    b32 supported;    // the device can run the production path and it was not disabled
    b32 mesh_shaders; // rasterise with mesh shaders (else indirect draws)
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
    // The shading tiles: each one's class (written with the pyramid), and
    // the bins' lists of them.
    NphGpuBuffer tile_class; // u32 per tile
    NphGpuBuffer tile_lists; // u32[NPH_VIS_BINS][tiles]
    u32 shade_tiles_x;
    u32 shade_tiles_y;
    b32 hzb_full_rebuild; // tests: rebuild the whole pyramid after phase 1 too
    // That frame's view, for phase 0's tests against its HZB.
    NphMat4 prev_view;
    NphMat4 prev_view_proj;
    NphVec4 prev_projection;
    NphDVec3 prev_camera;

    NphRenderFrameStats stats_last; // the vis_* fields of the last read-back
    b32 fallback_logged;            // PRODUCTION requested without the path: said once
    u32 overflow_logged;            // overflow bits already reported
    b32 updates_logged;             // instance updates waited for a frame: said once
    VisFrame frame;
} RenderVis;

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
    NphGpuPipeline forward[3][RENDER_CULL_COUNT]; // [alpha mode][cull]
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
NphResult render_load_shader_file(NphRenderer *r, const char *file, NphGpuShader *out);
const RenderTexture *render_texture_get(const NphRenderer *r, NphRenderTexture texture);
u32 render_material_sampler(const NphRenderer *r, const NphRenderTextureBinding *binding);
// The pool slot of a live handle, or NPH_U32_MAX for null and stale ones.
u32 render_slot_of(const NphHandlePool *pool, u32 value);

// frame.c
void render_frame_layout(const NphRendererDesc *desc, RenderFrameLayout *out);
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

// vis.c: the production path's pipelines and frames.
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
NphResult render_vis_frame(NphRenderer *r, const NphRenderView *view, NphGpuRenderGraph *graph,
                           u32 width, u32 height, const NphMat4 *view_m, const NphMat4 *proj,
                           NphGpuRgImage env, NphGpuRgImage specular, NphGpuRgImage lut,
                           NphGpuRgBuffer sh, NphGpuRgImage *out_hdr, NphRenderFrameStats *stats);
// The bindless storage index of the frame's visibility buffer, for the
// tonemap pass's depth-tested debug lines (valid inside callbacks).
u32 render_vis_visbuffer_index(const NphRenderer *r, NphGpuRenderGraph *graph);
// The transparent surfaces of a production frame, after render_vis_frame:
// the visibility buffer's depth into a depth attachment, then `draw` (the
// forward pipelines' blended draws) over the HDR image.
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

// SPDX-License-Identifier: Apache-2.0
#pragma once

// Internal state of the renderer (renderer.c, frame.c, ibl.c,
// debug_draw.c). See render/renderer.h for the contract and docs/render.md
// for the design.

#include <nepheline/core/atomic.h>
#include <nepheline/core/handle.h>
#include <nepheline/render/debug_draw.h>
#include <nepheline/render/renderer.h>

#include <nph_render.h>

#define RENDER_NAME_MAX 64U

// The formats of the renderer's own images.
#define RENDER_HDR_FORMAT   NPH_GPU_FORMAT_R16G16B16A16_SFLOAT
#define RENDER_DEPTH_FORMAT NPH_GPU_FORMAT_D32_SFLOAT
#define RENDER_IBL_FORMAT   NPH_GPU_FORMAT_R16G16B16A16_SFLOAT

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
// faces clockwise on screen).
typedef enum RenderCull {
    RENDER_CULL_BACK_CCW = 0,
    RENDER_CULL_BACK_CW = 1,
    RENDER_CULL_NONE = 2,
    RENDER_CULL_COUNT = 3
} RenderCull;

typedef struct RenderTargetPipelines {
    NphGpuFormat format; // UNDEFINED: unused entry
    NphGpuPipeline tonemap;
    NphGpuPipeline debug_depth;
    NphGpuPipeline debug_overlay;
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
} RenderMesh;

typedef struct RenderMaterial {
    u32 handle;
    NphRenderMaterialDesc desc; // name cleared; handles resolved per frame
} RenderMaterial;

typedef struct RenderInstance {
    u32 handle;
    NphRenderMesh mesh;
    NphRenderMaterial *materials; // from the renderer's allocator
    u32 material_count;
    NphRenderTransform transform;
    b32 hidden;
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
    u64 size;
} RenderFrameLayout;

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

    // The frame being recorded (read by the pass callbacks).
    NphRenderPush push;
    NphRenderTonemapPush tonemap_push;
    const RenderTargetPipelines *frame_target;
    NphGpuRgImage hdr_rg; // the frame's HDR target, resolved in the tonemap pass
    u32 frame_slot;
};

// The record at `offset` in the upload buffer of the frame being recorded,
// as a void pointer so that it converts to the record's type.
NPH_INLINE void *render_frame_at(const NphRenderer *r, u64 offset)
{
    return r->frame_maps[r->frame_slot] + offset;
}

// renderer.c
NphResult render_load_shader(NphRenderer *r, RenderShader shader, const char *file);
const RenderTexture *render_texture_get(const NphRenderer *r, NphRenderTexture texture);
u32 render_material_sampler(const NphRenderer *r, const NphRenderTextureBinding *binding);

// frame.c
void render_frame_layout(const NphRendererDesc *desc, RenderFrameLayout *out);

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

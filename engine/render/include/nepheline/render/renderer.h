// SPDX-License-Identifier: Apache-2.0
#pragma once

// The forward PBR reference renderer (CLAUDE.md §8.6 stages 1-2;
// docs/render.md; docs/adr/0018-reference-renderer.md).
//
// This renderer is the ground truth that golden images are compared
// against, not the production path: it draws every visible submesh with
// its own draw call and evaluates every light per pixel, so its CPU cost
// grows with the object count and its GPU cost with lights times pixels.
// The GPU-driven visibility-buffer renderer of §8.6 stages 3-8 (Phase 10)
// is checked against it.
//
// What it renders:
//   - glTF 2.0 metallic-roughness materials: base colour, metallic,
//     roughness, normal, occlusion, and emissive textures; opaque, masked,
//     and blended alpha; double-sided and unlit materials;
//   - punctual lights in KHR_lights_punctual units (lux, candela);
//   - image-based lighting from a procedural sky, an equirectangular
//     texture, or a cube texture, through the split-sum approximation;
//   - a pre-exposed HDR target (physical exposure from EV100), tonemapped
//     with ACES (Hill's fit) or AgX into the caller's target image;
//   - debug lines (debug_draw.h) over the tonemapped image.
//
// Large worlds (CLAUDE.md §8.2): instance, light, and camera positions are
// 64-bit (NphDVec3); everything reaches the GPU relative to the camera in
// 32-bit floats, so precision holds at any distance from the origin.
//
// Resources are handles (32-bit, generational). Textures and meshes are
// uploaded once at creation through the device's staging ring and are
// immutable; materials, instances, and lights are CPU records copied to
// per-frame GPU buffers, so editing them never races a frame in flight.
//
// Each frame, between nph_gpu_frame_begin and the graph's compile, the
// caller begins the render graph and calls nph_render_frame, which
// declares the renderer's passes into it (image-based-lighting
// precomputation when the environment changed, the forward pass, the
// tonemap and debug pass) ending in the caller's target image. The caller
// may add passes of its own before compiling, such as a UI or a present.
//
// Costs the caller can see (§1.4): create allocates every pool from the
// description's capacities and creates the GPU objects; texture, mesh,
// and instance creation allocate (the instance's material table) from the
// renderer's allocator and the meshes and textures their GPU memory; a
// frame allocates nothing. nph_render_frame copies the frame's records
// into mapped GPU memory and sorts the draws (qsort, no allocation).
//
// Thread-safety: NPH_MAIN_THREAD_ONLY (the thread that owns the frame),
// except for the debug-draw functions (debug_draw.h).

#include <nepheline/gpu/render_graph.h>
#include <nepheline/math/geometry.h>
#include <nepheline/math/quat.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_CATEGORY_DECLARE(NPH_LOG_RENDER);

typedef struct NphRenderer NphRenderer;

// ---------------------------------------------------------------------------
// Handles
// ---------------------------------------------------------------------------

typedef struct NphRenderTexture {
    u32 value;
} NphRenderTexture;
typedef struct NphRenderMesh {
    u32 value;
} NphRenderMesh;
typedef struct NphRenderMaterial {
    u32 value;
} NphRenderMaterial;
typedef struct NphRenderInstance {
    u32 value;
} NphRenderInstance;
typedef struct NphRenderLight {
    u32 value;
} NphRenderLight;

#define NPH_RENDER_TEXTURE_NULL  ((NphRenderTexture){0U})
#define NPH_RENDER_MESH_NULL     ((NphRenderMesh){0U})
#define NPH_RENDER_MATERIAL_NULL ((NphRenderMaterial){0U})
#define NPH_RENDER_INSTANCE_NULL ((NphRenderInstance){0U})
#define NPH_RENDER_LIGHT_NULL    ((NphRenderLight){0U})

// ---------------------------------------------------------------------------
// Creation
// ---------------------------------------------------------------------------

typedef struct NphRendererDesc {
    NphAllocator *alloc;    // required: pools, CPU records, instance material tables
    NphGpuDevice *dev;      // required
    const char *shader_dir; // required: where the renderer's .spv files are
    // Capacities; zero picks the default in parentheses. Hard limits:
    // creation past them returns NPH_RESULT_FULL.
    u32 max_textures;  // (1024)
    u32 max_meshes;    // (1024)
    u32 max_materials; // (1024)
    u32 max_instances; // (16384)
    u32 max_lights;    // (256): all evaluated for every pixel
    u32 max_draws;     // visible submeshes per frame (16384); the rest are dropped and logged
    u32 debug_lines;   // per frame, depth-tested and overlay each (32768)
    // Image-based lighting resolution; zero picks the default.
    u32 environment_size; // level-0 face extent of the environment cube (256), a power of two
    u32 specular_size;    // level-0 face extent of the prefiltered cube (128), a power of two
    u32 specular_mips;    // roughness levels of the prefiltered cube (6), at most log2(size) + 1
    u32 specular_samples; // GGX samples per texel when prefiltering (256)
    u32 brdf_lut_size;    // extent of the DFG table (128)
    // Anisotropic filtering for material textures: 0 picks 8; 1 turns it
    // off (golden-image tests do, since anisotropic filtering is
    // implementation-defined).
    f32 max_anisotropy;
    const char *name; // for logs and GPU object names
} NphRendererDesc;

// Creates the renderer: pools, samplers, the per-frame buffers, the
// image-based-lighting images (a neutral environment until one is set),
// and every pipeline. NPH_RESULT_INVALID_ARGUMENT for a missing allocator,
// device, or shader directory, or a size that is not a power of two;
// NPH_RESULT_NOT_FOUND or NPH_RESULT_IO_ERROR when a shader cannot be
// read; NPH_RESULT_OUT_OF_MEMORY otherwise. `*out` is NULL on failure.
NPH_MUST_USE NphResult nph_renderer_create(const NphRendererDesc *desc, NphRenderer **out);

// Waits for the device to go idle and releases every resource the
// renderer created, including any the caller did not destroy (reported at
// warning level). Safe on NULL.
void nph_renderer_destroy(NphRenderer *r);

NphGpuDevice *nph_renderer_device(NphRenderer *r);

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

typedef enum NphRenderTextureKind {
    NPH_RENDER_TEXTURE_2D = 0,
    NPH_RENDER_TEXTURE_CUBE, // layer_count 6: +X -X +Y -Y +Z -Z
} NphRenderTextureKind;

typedef enum NphRenderTextureFlags {
    // Two-channel tangent-space normals (BC5, RG8): z is reconstructed.
    NPH_RENDER_TEXTURE_FLAG_NORMAL_TWO_CHANNEL = 1U << 0U,
} NphRenderTextureFlags;

typedef struct NphRenderTextureDesc {
    NphRenderTextureKind kind;
    NphGpuFormat
        format; // any sampled format; BC formats need NphGpuFeatures.texture_compression_bc
    u32 width;
    u32 height;
    u32 mip_count;   // levels provided in `mips` (1..NPH_GPU_MAX_MIPS)
    u32 layer_count; // 0: 1 (2D) or 6 (cube)
    // Tightly packed texels of each level, every layer of the level in
    // order (the layout of a cooked texture's mip blobs).
    const void *const *mips;
    const u64 *mip_sizes;
    u32 flags; // NphRenderTextureFlags
    const char *name;
} NphRenderTextureDesc;

// Creates the image and queues the upload of every level (completed before
// the next frame's passes run). NPH_RESULT_INVALID_ARGUMENT for a size
// that does not match the format and extent; NPH_RESULT_UNSUPPORTED for a
// BC format without the feature; NPH_RESULT_FULL past max_textures.
NPH_MUST_USE NphResult nph_render_texture_create(NphRenderer *r, const NphRenderTextureDesc *desc,
                                                 NphRenderTexture *out);
// Destruction is deferred past the frames in flight. Materials that still
// name the texture draw without it (logged once). Null handles are ignored.
void nph_render_texture_destroy(NphRenderer *r, NphRenderTexture texture);
bool nph_render_texture_is_valid(const NphRenderer *r, NphRenderTexture texture);

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

// The vertex streams, bit for bit those of a cooked mesh (engine/asset
// mesh.h: NphMeshPosition, NphMeshAttrib), so cooked data is handed over
// without conversion.
typedef struct NphRenderVertexPosition {
    u16 x, y, z; // unorm16 across the mesh's quantisation box
    u16 reserved;
} NphRenderVertexPosition;

typedef struct NphRenderVertexAttrib {
    u32 normal;  // octahedral snorm16x2 (math/pack.h nph_oct_encode_snorm16)
    u32 tangent; // snorm8x4: xyz direction, w handedness (+1 or -1)
    u32 uv0;     // f16x2
    u32 color;   // RGBA8 unorm, linear (0xFFFFFFFF when absent)
} NphRenderVertexAttrib;

NPH_STATIC_ASSERT(sizeof(NphRenderVertexPosition) == 8U, "a position is 8 bytes");
NPH_STATIC_ASSERT(sizeof(NphRenderVertexAttrib) == 16U, "an attribute record is 16 bytes");

typedef struct NphRenderSubmesh {
    u32 index_offset;  // into the index array
    u32 index_count;   // a multiple of 3
    u32 material_slot; // index into the instance's material table
} NphRenderSubmesh;

#define NPH_RENDER_MAX_SUBMESHES 256U

typedef struct NphRenderMeshDesc {
    u32 vertex_count;
    u32 index_count;
    const NphRenderVertexPosition *positions;
    const NphRenderVertexAttrib *attribs;
    const u32 *indices; // triangle list
    const NphRenderSubmesh *submeshes;
    u32 submesh_count; // 1..NPH_RENDER_MAX_SUBMESHES
    // Dequantisation: position = offset + unorm16 / 65535 * scale.
    NphVec3 position_offset;
    NphVec3 position_scale;
    NphAABB bounds; // of the dequantised positions (culling)
    const char *name;
} NphRenderMeshDesc;

// Uploads the streams. NPH_RESULT_INVALID_ARGUMENT for empty streams, an
// index count that is not a multiple of 3, an index or submesh range out
// of bounds (every index is checked), or too many submeshes;
// NPH_RESULT_FULL past max_meshes.
NPH_MUST_USE NphResult nph_render_mesh_create(NphRenderer *r, const NphRenderMeshDesc *desc,
                                              NphRenderMesh *out);
// Deferred like textures. Instances of the mesh must be destroyed first
// (asserted; refused with an error logged).
void nph_render_mesh_destroy(NphRenderer *r, NphRenderMesh mesh);
bool nph_render_mesh_is_valid(const NphRenderer *r, NphRenderMesh mesh);

// ---------------------------------------------------------------------------
// Materials
// ---------------------------------------------------------------------------

typedef enum NphRenderAlphaMode {
    NPH_RENDER_ALPHA_OPAQUE = 0,
    NPH_RENDER_ALPHA_MASK = 1,
    NPH_RENDER_ALPHA_BLEND = 2,
} NphRenderAlphaMode;

typedef enum NphRenderWrap {
    NPH_RENDER_WRAP_REPEAT = 0,
    NPH_RENDER_WRAP_CLAMP = 1,
    NPH_RENDER_WRAP_MIRROR = 2,
} NphRenderWrap;

// The material texture slots (glTF 2.0).
typedef enum NphRenderTextureSlot {
    NPH_RENDER_SLOT_BASE_COLOR = 0,     // sRGB format expected
    NPH_RENDER_SLOT_METALLIC_ROUGHNESS, // roughness in G, metallic in B
    NPH_RENDER_SLOT_NORMAL,
    NPH_RENDER_SLOT_OCCLUSION, // R
    NPH_RENDER_SLOT_EMISSIVE,  // sRGB format expected
    NPH_RENDER_SLOT_COUNT
} NphRenderTextureSlot;

typedef struct NphRenderTextureBinding {
    NphRenderTexture texture; // null: the slot is unused
    NphRenderWrap wrap_u;
    NphRenderWrap wrap_v;
    b32 nearest; // nearest filtering instead of trilinear
} NphRenderTextureBinding;

typedef struct NphRenderMaterialDesc {
    NphVec4 base_color; // linear RGBA
    NphVec3 emissive;   // linear radiance, cd/m^2 (glTF factor times strength, in nits)
    f32 metallic;
    f32 roughness; // perceptual
    f32 normal_scale;
    f32 occlusion_strength;
    f32 alpha_cutoff;
    NphRenderAlphaMode alpha_mode;
    b32 double_sided;
    b32 unlit; // KHR_materials_unlit
    NphRenderTextureBinding textures[NPH_RENDER_SLOT_COUNT];
    const char *name;
} NphRenderMaterialDesc;

// glTF's defaults: white, non-metallic, fully rough, opaque, no textures.
void nph_render_material_desc_default(NphRenderMaterialDesc *desc);

// NPH_RESULT_INVALID_ARGUMENT for a stale texture handle or out-of-range
// enum values; NPH_RESULT_FULL past max_materials.
NPH_MUST_USE NphResult nph_render_material_create(NphRenderer *r, const NphRenderMaterialDesc *desc,
                                                  NphRenderMaterial *out);
// Replaces the description; visible from the next frame.
NPH_MUST_USE NphResult nph_render_material_update(NphRenderer *r, NphRenderMaterial material,
                                                  const NphRenderMaterialDesc *desc);
// Instances that still name the material draw with the default material.
void nph_render_material_destroy(NphRenderer *r, NphRenderMaterial material);
bool nph_render_material_is_valid(const NphRenderer *r, NphRenderMaterial material);

// ---------------------------------------------------------------------------
// Instances
// ---------------------------------------------------------------------------

// A world transform with a 64-bit translation: the linear part (rotation,
// scale, and any shear from a hierarchy) and the position.
typedef struct NphRenderTransform {
    NphMat3 linear;
    NphDVec3 translation;
} NphRenderTransform;

NphRenderTransform nph_render_transform_identity(void);
// From a 32-bit affine transform (an NphMat4's last row is ignored).
NphRenderTransform nph_render_transform_from_mat4(const NphMat4 *m);
NphRenderTransform nph_render_transform_from_trs(NphDVec3 translation, NphQuat rotation,
                                                 NphVec3 scale);

typedef struct NphRenderInstanceDesc {
    NphRenderMesh mesh;
    // One material per material slot of the mesh's submeshes; a slot past
    // the count, or a null handle, draws with the default material.
    const NphRenderMaterial *materials;
    u32 material_count;
    NphRenderTransform transform;
    b32 hidden;
} NphRenderInstanceDesc;

// NPH_RESULT_INVALID_ARGUMENT for a stale mesh or material handle or a
// transform with a zero determinant; NPH_RESULT_FULL past max_instances;
// NPH_RESULT_OUT_OF_MEMORY when the material table cannot be allocated.
NPH_MUST_USE NphResult nph_render_instance_create(NphRenderer *r, const NphRenderInstanceDesc *desc,
                                                  NphRenderInstance *out);
NPH_MUST_USE NphResult nph_render_instance_set_transform(NphRenderer *r, NphRenderInstance instance,
                                                         const NphRenderTransform *transform);
void nph_render_instance_set_hidden(NphRenderer *r, NphRenderInstance instance, bool hidden);
void nph_render_instance_destroy(NphRenderer *r, NphRenderInstance instance);
bool nph_render_instance_is_valid(const NphRenderer *r, NphRenderInstance instance);

// ---------------------------------------------------------------------------
// Lights (KHR_lights_punctual)
// ---------------------------------------------------------------------------

typedef enum NphRenderLightType {
    NPH_RENDER_LIGHT_TYPE_DIRECTIONAL = 0,
    NPH_RENDER_LIGHT_TYPE_POINT = 1,
    NPH_RENDER_LIGHT_TYPE_SPOT = 2,
} NphRenderLightType;

typedef struct NphRenderLightDesc {
    NphRenderLightType type;
    NphVec3 color;     // linear
    f32 intensity;     // lux (directional) or candela (point, spot)
    NphDVec3 position; // point and spot
    NphVec3 direction; // directional and spot: the direction the light travels
    f32 range;         // point and spot: 0 is unlimited
    f32 inner_cone;    // spot: radians, [0, outer)
    f32 outer_cone;    // spot: radians, (inner, pi / 2]
} NphRenderLightDesc;

// NPH_RESULT_INVALID_ARGUMENT for a negative intensity or range, a zero
// direction, or cone angles out of order; NPH_RESULT_FULL past max_lights.
NPH_MUST_USE NphResult nph_render_light_create(NphRenderer *r, const NphRenderLightDesc *desc,
                                               NphRenderLight *out);
NPH_MUST_USE NphResult nph_render_light_update(NphRenderer *r, NphRenderLight light,
                                               const NphRenderLightDesc *desc);
void nph_render_light_destroy(NphRenderer *r, NphRenderLight light);
bool nph_render_light_is_valid(const NphRenderer *r, NphRenderLight light);

// ---------------------------------------------------------------------------
// Environment (image-based lighting)
// ---------------------------------------------------------------------------

typedef enum NphRenderEnvironmentSource {
    NPH_RENDER_ENV_NONE = 0, // black: no image-based lighting
    NPH_RENDER_ENV_SKY,      // the procedural sky below
    NPH_RENDER_ENV_EQUIRECT, // a 2D texture in latitude-longitude layout
    NPH_RENDER_ENV_CUBE,     // a cube texture
} NphRenderEnvironmentSource;

// A gradient sky: radiance (cd/m^2) from the horizon to the zenith as
// mix(horizon, zenith, h^zenith_exponent) for elevation sine h >= 0, and
// towards `ground` below the horizon over 1 / ground_sharpness of h.
// There is no sun disk: the sun is a directional light.
typedef struct NphRenderSkyDesc {
    NphVec3 zenith;
    NphVec3 horizon;
    NphVec3 ground;
    f32 zenith_exponent;  // 0: 0.5
    f32 ground_sharpness; // 0: 8
} NphRenderSkyDesc;

// A clear daylight sky for EV100 around 14-15 (zenith about 6000 cd/m^2).
void nph_render_sky_desc_default(NphRenderSkyDesc *desc);

typedef struct NphRenderEnvironmentDesc {
    NphRenderEnvironmentSource source;
    NphRenderSkyDesc sky;     // NPH_RENDER_ENV_SKY
    NphRenderTexture texture; // NPH_RENDER_ENV_EQUIRECT (2D) or NPH_RENDER_ENV_CUBE (cube)
    f32 intensity;            // multiplier on the radiance (0: 1)
} NphRenderEnvironmentDesc;

// Replaces the environment. Its cube, prefiltered cube, and irradiance
// are recomputed on the GPU by the next nph_render_frame (a few compute
// passes, once). The texture must stay alive until that frame has been
// declared. NPH_RESULT_INVALID_ARGUMENT for a stale texture or one of the
// wrong kind.
NPH_MUST_USE NphResult nph_render_set_environment(NphRenderer *r,
                                                  const NphRenderEnvironmentDesc *desc);

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------

// A perspective camera looking down its local -Z with +Y up (ADR-0008),
// with an infinite reverse-Z projection.
typedef struct NphRenderCamera {
    NphDVec3 position;
    NphQuat orientation;
    f32 yfov;  // radians
    f32 znear; // metres, > 0
} NphRenderCamera;

// A camera at `eye` looking at `target` with `up` as the vertical hint.
NphRenderCamera nph_render_camera_look_at(NphDVec3 eye, NphDVec3 target, NphVec3 up, f32 yfov,
                                          f32 znear);

typedef enum NphRenderTonemapper {
    NPH_RENDER_TONEMAPPER_NONE = 0, // clamp
    NPH_RENDER_TONEMAPPER_ACES = 1, // Hill's ACES fit
    NPH_RENDER_TONEMAPPER_AGX = 2,  // AgX, base contrast
} NphRenderTonemapper;

typedef enum NphRenderBackground {
    NPH_RENDER_BACKGROUND_ENV = 0,   // the environment cube
    NPH_RENDER_BACKGROUND_SOLID = 1, // background_color
} NphRenderBackground;

typedef enum NphRenderDebugView {
    NPH_RENDER_VIEW_LIT = 0,
    NPH_RENDER_VIEW_BASE_COLOR,
    NPH_RENDER_VIEW_NORMAL,
    NPH_RENDER_VIEW_ROUGHNESS,
    NPH_RENDER_VIEW_METALLIC,
    NPH_RENDER_VIEW_OCCLUSION,
    NPH_RENDER_VIEW_DIRECT,   // punctual lights only
    NPH_RENDER_VIEW_INDIRECT, // image-based lighting only
    NPH_RENDER_VIEW_COUNT
} NphRenderDebugView;

typedef struct NphRenderView {
    NphRenderCamera camera;
    // Exposure as EV100 (Lagarde & de Rousiers, "Moving Frostbite to PBR",
    // SIGGRAPH 2014): radiance is scaled by 1 / (1.2 * 2^ev100). About 15
    // for a sunny exterior, 7-9 for interiors.
    f32 ev100;
    NphRenderTonemapper tonemapper;
    NphRenderBackground background;
    NphVec3 background_color; // radiance, cd/m^2
    NphRenderDebugView debug_view;
    b32 disable_ibl;
} NphRenderView;

// A view of `camera`: EV100 15, ACES, the environment as background.
void nph_render_view_default(NphRenderView *view, NphRenderCamera camera);

// The linear exposure for an EV100 value.
f32 nph_render_exposure_from_ev100(f32 ev100);

typedef struct NphRenderFrameStats {
    u32 instances; // live instances
    u32 culled;    // of those, outside the frustum or hidden
    u32 draws;     // submesh draws issued (opaque + masked + blended)
    u32 blended;   // of those, blended
    u32 dropped;   // visible draws past max_draws
    u32 triangles; // drawn
    u32 lights;    // evaluated per pixel
    u32 debug_lines;
    b32 environment_updated; // this frame declared the IBL passes
} NphRenderFrameStats;

// Declares this frame's passes into `graph` (between nph_gpu_rg_begin and
// nph_gpu_rg_compile) and fills the frame's GPU records. `target` is the
// image the tonemapped result is written to, typically the acquired
// swapchain image: its extent is the render extent, and it must be 2D with
// COLOR_ATTACHMENT usage in R8G8B8A8 or B8G8R8A8 (UNORM or sRGB) or
// A2B10G10R10_UNORM. The renderer imports it and returns its graph handle
// in `out_target` (may be NULL) for the caller's own passes and the
// present. Uploads queued since the last frame are flushed so the passes
// see them. Once per frame, inside a frame (nph_gpu_frame_begin), from the
// main thread.
// NPH_RESULT_INVALID_STATE outside a frame or for a second call in one
// frame; NPH_RESULT_INVALID_ARGUMENT for an invalid target or a camera
// without a positive field of view and near distance;
// NPH_RESULT_UNSUPPORTED for a target format the tonemapper cannot write;
// NPH_RESULT_FULL when the graph's capacities are exhausted.
// `out_stats` may be NULL.
NPH_MUST_USE NphResult nph_render_frame(NphRenderer *r, const NphRenderView *view,
                                        NphGpuRenderGraph *graph, NphGpuImage target,
                                        NphGpuRgImage *out_target, NphRenderFrameStats *out_stats);

#ifdef __cplusplus
}
#endif

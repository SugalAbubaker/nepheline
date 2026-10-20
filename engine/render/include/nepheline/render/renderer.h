// SPDX-License-Identifier: Apache-2.0
#pragma once

// The renderer (CLAUDE.md §8.6; docs/render.md): one set of resources
// drawn by either of two pipelines, chosen per view (NphRenderPipeline):
//
//   - REFERENCE, the forward PBR reference renderer (§8.6 stages 1-2;
//     docs/adr/0018-reference-renderer.md): the ground truth golden images
//     are compared against. It draws every visible submesh with its own
//     draw call and evaluates every light per pixel, so its CPU cost grows
//     with the object count and its GPU cost with lights times pixels.
//   - PRODUCTION, the GPU-driven visibility-buffer renderer (§8.6 stages
//     3 and on; docs/adr/0029-gpu-driven-renderer.md): the scene lives in
//     GPU tables updated only where something changed; instances and
//     meshlets are culled on the GPU (frustum, normal cone, two-phase
//     occlusion against a hierarchical depth buffer); visible meshlets are
//     rasterised into a 64-bit visibility buffer by mesh shaders, or by
//     indirect draws on devices without them; one compute pass shades
//     every pixel with the reference's own shading, each pixel lighting
//     with the lights its distance bin and screen tile may meet (a light
//     grid built on the GPU each frame from a persistent light table). Its
//     CPU cost per frame grows with neither the number of objects nor of
//     lights. It needs 64-bit image atomics, subgroup ballot, arithmetic,
//     and shuffle, and gl_PrimitiveID in fragment shaders
//     (nph_renderer_production_supported).
//
// What it renders:
//   - glTF 2.0 metallic-roughness materials: base colour, metallic,
//     roughness, normal, occlusion, and emissive textures; opaque, masked,
//     and blended alpha; double-sided and unlit materials;
//   - punctual lights in KHR_lights_punctual units (lux, candela), with
//     ray-traced hard shadows for those that cast them where the device
//     has ray queries (nph_renderer_ray_tracing_supported);
//   - image-based lighting from a procedural sky, an equirectangular
//     texture, or a cube texture, through the split-sum approximation;
//   - a pre-exposed HDR target (physical exposure from EV100), tonemapped
//     with ACES (Hill's fit) or AgX into the caller's target image;
//   - on the production path, temporal anti-aliasing at native resolution
//     (NphRenderAntiAliasing), fused with the tonemap, and motion blur
//     (NphRenderView.shutter);
//   - depth of field through a thin lens, bloom, and an automatically
//     metered exposure, on both pipelines;
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
#include <nepheline/render/meshlet.h>

#ifdef __cplusplus
extern "C" {
#endif

NPH_LOG_ENGINE_CATEGORY_DECLARE(NPH_LOG_RENDER);

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
    u32 max_lights;    // (256), at most NPH_RENDER_MAX_LIGHTS (4096)
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
    // The production path (zero picks the default in parentheses).
    b32 disable_production;   // never create it: no GPU memory or pipelines for it
    u32 max_visible_clusters; // meshlets that survive culling per frame (524288)
    u32 max_cull_meshlets;    // meshlets of the instances phase 0 keeps, per frame (2^20)
    u32 max_deferred;         // instances phase 0 defers, and phase 1's meshlets: those
                              // phase 0 deferred plus those of deferred instances now
                              // visible (262144)
    f32 sw_raster_pixels;     // clusters whose triangles average at most this many pixels
                              // across (and that fit in 64) rasterise in software (3)
    u32 max_instance_updates; // instance records sent to the GPU per frame (16384); the
                              // rest wait for the next frame
    u32 max_material_slots;   // material slots across all instances (4 * max_instances);
                              // a destroyed instance's slots return two frames later
    u32 meshlet_index_pool;   // meshlet corners of all meshes for the indirect raster, on
                              // devices without mesh shaders (2^24); a destroyed mesh's
                              // return two frames later
    // Ray-traced shadows, where the device has ray queries (docs/render.md
    // "Shadows"): set to never create them (no acceleration structures,
    // GPU memory, or pipelines for them); lights that cast shadows then draw
    // unshadowed (logged once).
    b32 disable_ray_tracing;
    // The production path's virtual shadow map for its sun, the first live
    // directional light that casts shadows (docs/render.md "Shadows"),
    // where the device has depth clamping and clip distances. Zero picks
    // the default in parentheses.
    b32 disable_virtual_shadows; // never create it: the sun's shadows are then ray traced
    u32 vsm_pages;               // physical pages of 128 x 128 depth texels, 64 KB each (2048)
    u32 max_vsm_meshlets;        // caster meshlets tested against the pages drawn in a frame (2^20)
    u32 max_vsm_draws;           // (meshlet, page) draws in a frame (2^20)
    // The production path's radiance cache (docs/render.md "Global
    // illumination"), where the device has ray queries: its cells, a power of
    // two of at least 16 (262144: 9 MB), and the rays its update traces a
    // frame, a budget whatever the resolution (65536). Zero picks the default.
    b32 disable_gi; // never create it
    u32 gi_cells;
    u32 gi_rays;
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
    // Meshlets (render/meshlet.h), as a cooked mesh carries them; with
    // `meshlets` NULL the renderer builds them (a CPU cost at creation:
    // about half a microsecond per triangle). Each meshlet's submesh
    // indexes `submeshes`, and the meshlets of a submesh cover exactly its
    // triangles.
    const NphRenderMeshlet *meshlets;
    const NphRenderMeshletBounds *meshlet_bounds;
    u32 meshlet_count;
    const u32 *meshlet_vertices; // indices into the vertices
    u32 meshlet_vertex_count;
    const u8 *meshlet_triangles; // meshlet-local corners, three per triangle
    u32 meshlet_triangle_count;
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
    // Casts shadows: shadow rays where the device has ray queries (both
    // pipelines; docs/render.md "Shadows"), none elsewhere (logged once).
    b32 shadows;
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
    // The diffuse irradiance the indirect light takes (the environment's, or
    // the production path's radiance cache's), over pi: what a white surface
    // would reflect of it, pre-exposed.
    NPH_RENDER_VIEW_IRRADIANCE,
    // The production path's culling and raster, as a colour per visible
    // cluster, triangle, or instance (the reference path shades as LIT).
    NPH_RENDER_VIEW_CLUSTERS,
    NPH_RENDER_VIEW_TRIANGLES,
    NPH_RENDER_VIEW_INSTANCES,
    NPH_RENDER_VIEW_COUNT
} NphRenderDebugView;

typedef enum NphRenderPipeline {
    NPH_RENDER_PIPELINE_REFERENCE = 0,  // the forward reference renderer
    NPH_RENDER_PIPELINE_PRODUCTION = 1, // the GPU-driven visibility-buffer renderer
} NphRenderPipeline;

// Switches of the production path, for tests and debugging. The culling's
// leave the image as it is and change only the work; RT_SHADOWS shadows
// the sun with rays (as the reference does) instead of its virtual shadow
// map.
typedef enum NphRenderVisFlags {
    NPH_RENDER_VIS_NO_OCCLUSION = 1U << 0U,  // no hierarchical-depth tests
    NPH_RENDER_VIS_NO_CONE = 1U << 1U,       // no meshlet normal-cone tests
    NPH_RENDER_VIS_NO_SW_RASTER = 1U << 2U,  // every cluster through the hardware raster
    NPH_RENDER_VIS_NO_LIGHT_GRID = 1U << 3U, // every light for every pixel, as the reference
    NPH_RENDER_VIS_RT_SHADOWS = 1U << 4U,    // no virtual shadow map: the sun's shadows ray traced
} NphRenderVisFlags;

// How a view's exposure is set.
typedef enum NphRenderExposureMode {
    NPH_RENDER_EXPOSURE_MANUAL = 0, // `ev100`
    // Metered from the frame's luminance and adapted toward it over time,
    // starting from `ev100` (docs/render.md "Bloom and exposure"). The
    // metering reaches the exposure NPH_GPU_FRAMES_IN_FLIGHT frames late;
    // debug views are not metered (the exposure holds its course).
    NPH_RENDER_EXPOSURE_AUTO = 1,
} NphRenderExposureMode;

// How a view is anti-aliased.
typedef enum NphRenderAntiAliasing {
    // One sample per pixel, at its centre: every frame stands alone.
    NPH_RENDER_AA_NONE = 0,
    // The production path's temporal anti-aliasing at native resolution
    // (docs/render.md "Temporal anti-aliasing"): each frame's projection
    // moves by a sub-pixel offset, and the frame is blended into the history
    // of the earlier ones, read where what each pixel sees was a frame ago.
    // The reference renderer ignores it, and so do debug views.
    NPH_RENDER_AA_TEMPORAL = 1,
} NphRenderAntiAliasing;

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
    // The pipeline that draws the view. PRODUCTION falls back to REFERENCE
    // (logged once) on a renderer without the production path.
    NphRenderPipeline pipeline;
    u32 vis_flags; // NphRenderVisFlags
    NphRenderAntiAliasing anti_aliasing;
    // The view does not continue the previous frame's (a cut, a teleport,
    // another camera): temporal history is dropped, not reprojected.
    b32 camera_cut;
    NphRenderExposureMode exposure;
    f32 exposure_compensation; // AUTO: stops brighter than the metered exposure
    f32 exposure_speed;        // AUTO: e-foldings per second toward the metering (0: 1.5)
    f32 frame_seconds;         // the time since the previous frame, AUTO's pace (0: 1/240)
    f32 bloom; // the bloom's weight in the image, 0 to 1 (0: none; debug views have none)
    // Motion blur (the production path; docs/render.md "Motion blur"): how
    // long the shutter is open, in seconds (0: no blur). Each pixel blurs
    // over what moves across it while the shutter is open, centred on the
    // frame: its motion since the previous frame times shutter /
    // frame_seconds, reaching at most NPH_RENDER_MB_TILE (32) pixels either
    // way. Only a frame that follows a production frame of the same extent
    // without a cut blurs; debug views and the reference renderer never do.
    // A 180-degree shutter at 240 frames a second is 1/480 s.
    f32 shutter;
    // Depth of field (both pipelines; docs/render.md "Depth of field"): a
    // thin lens of f-number `aperture` (0: a pinhole, everything sharp)
    // focused `focus_distance` metres away, which must be more than its
    // focal length, in front of a sensor `sensor_height` metres tall (0:
    // 0.024, full frame); the focal length follows from the camera's yfov.
    // A point z metres away blurs into a circle of confusion of radius
    // f^2 |z - focus| / (2 N z (focus - f)) on the sensor, at most
    // NPH_RENDER_DOF_MAX_RADIUS (32) pixels. Debug views have none.
    f32 aperture;
    f32 focus_distance;
    f32 sensor_height;
    // A point on the lens, metres along the camera's x and y, that the frame
    // is seen through instead of its centre: the camera moves there and the
    // projection shears so the plane `focus_distance` away stays put.
    // Averaging frames over the aperture's disc renders depth of field as a
    // real lens does (an accumulation buffer; Haeberli & Akeley, "The
    // Accumulation Buffer", SIGGRAPH 1990): for captures, and the tests'
    // ground truth. Zero: the lens's centre.
    NphVec2 lens_offset;
    // Ambient occlusion (the production path; docs/render.md "Ambient
    // occlusion"): the radius in metres within which surfaces occlude the
    // environment's light (0: none). Ground-truth ambient occlusion from the
    // depth the view sees, which darkens the environment's light, the diffuse
    // part with what the occluders reflect back: made for lit views and the
    // indirect debug view when the environment lights the view, and for the
    // occlusion debug view. The reference renderer has none.
    f32 ao_radius;
    // Global illumination (the production path where the device has ray
    // queries; docs/render.md "Global illumination"): the diffuse light
    // takes its irradiance from a radiance cache in the world, which a fixed
    // budget of rays a frame keeps up to date: the environment's light
    // occluded by the scene, and the sun's and the environment's light the
    // scene reflects, bounce after bounce, instead of the environment's
    // light alone. Within `ao_radius` the ambient occlusion darkens it
    // instead. Made for lit views and the indirect and irradiance debug
    // views; the reference renderer has none.
    b32 gi;
} NphRenderView;

// A view of `camera`: EV100 15 (manual), ACES, the environment as
// background, no anti-aliasing, no bloom, no motion blur, a pinhole, no
// ambient occlusion, no global illumination (the runtime turns temporal
// anti-aliasing, bloom, ambient occlusion, and global illumination on,
// r.taa, r.bloom, r.ao, and r.gi, and gives the camera's shutter and lens,
// r.motion_blur and r.dof).
void nph_render_view_default(NphRenderView *view, NphRenderCamera camera);

// The focal length in metres of the view's lens: its sensor's height (0:
// 0.024) over 2 tan(yfov / 2). A view with an aperture or a lens offset must
// focus beyond it. NPH_THREADSAFE.
f32 nph_render_view_focal_length(const NphRenderView *view);

// The linear exposure for an EV100 value.
f32 nph_render_exposure_from_ev100(f32 ev100);

typedef struct NphRenderFrameStats {
    u32 instances; // live instances
    u32 culled;    // of those, outside the frustum or hidden
    u32 draws;     // submesh draws issued (opaque + masked + blended)
    u32 blended;   // of those, blended
    u32 dropped;   // visible draws past max_draws
    u32 triangles; // drawn
    u32 lights;    // live lights (the reference evaluates each for every pixel)
    u32 debug_lines;
    b32 environment_updated;    // this frame declared the IBL passes
    NphRenderPipeline pipeline; // the pipeline that drew the frame
    // Ray-traced shadows: whether the frame traced shadow rays (some light
    // casts shadows and the renderer has ray tracing); the meshes'
    // bottom-level structures it built (each once, in the first frame that
    // traces after the mesh's creation); and whether it rebuilt the top
    // level over the instances (the reference every frame; the production
    // path when instances changed, after a frame that traced nothing, or
    // when the camera moved more than 16 m from where it last built it).
    b32 shadow_rays;
    u32 shadow_builds;
    b32 shadow_rebuild;
    // The production path's GPU counters, read back from the frame that
    // completed NPH_GPU_FRAMES_IN_FLIGHT frames ago (vis_frame); zero
    // until one has. On that path `instances` above is the live count,
    // `culled` the hidden instances plus blended ones outside the frustum,
    // and `draws`, `blended`, and `triangles` count only the blended
    // surfaces the CPU draws itself; the GPU draws the rest.
    u64 vis_frame;
    u32 vis_clusters;    // meshlets that survived culling
    u32 vis_clusters_sw; // of those, rasterised in software
    u32 vis_triangles;   // in them all
    u32 vis_instances_culled_frustum;
    u32 vis_instances_culled_occlusion;
    u32 vis_meshlets_culled_frustum;
    u32 vis_meshlets_culled_cone;
    u32 vis_meshlets_culled_occlusion;
    u32 vis_lights;   // lights in view: those that reach every pixel, and the others the
                      // frustum keeps
    u32 vis_overflow; // NphRenderVisOverflow bits: lists that dropped work
    // The virtual shadow map: whether this frame has it, and the levels'
    // resolution bias it draws with (each step halves the resolution: the
    // renderer raises it while the map overflows its capacities and lowers
    // it again once there is room); then, read back with the counters above
    // (zero when that frame had none), the pages its pixels needed, the
    // pages drawn (new, or changed since), the pages the pool holds, those
    // taken from others, the caster draws, and NphRenderVsmOverflow bits.
    b32 vsm;
    u32 vsm_lod_bias;
    u32 vsm_requested;
    u32 vsm_drawn;
    u32 vsm_resident;
    u32 vsm_evicted;
    u32 vsm_draws;
    u32 vsm_overflow;
    // Temporal anti-aliasing: whether the frame was resolved temporally,
    // and whether it blended with history (not on the first such frame, nor
    // after a cut, a resize, or a frame resolved otherwise).
    b32 taa;
    b32 taa_history;
    // The exposure the frame was drawn with (for NPH_RENDER_EXPOSURE_AUTO the
    // adapted one) and the latest metering's target (0 until one is read
    // back), as EV100; whether the frame drew bloom.
    f32 ev100;
    f32 metered_ev100;
    b32 bloom;
    // Whether the frame was motion blurred, whether it had depth of field,
    // whether ambient occlusion, and whether global illumination.
    b32 motion_blur;
    b32 dof;
    b32 ao;
    b32 gi;
} NphRenderFrameStats;

// The virtual shadow map's capacities that can fill up
// (NphRenderFrameStats.vsm_overflow); each is in NphRendererDesc. Pixels
// whose pages did not fit in the pool are lit; a page whose draws did not
// all fit is drawn again the next frame. Either way the renderer raises the
// resolution bias until the map fits (NphRenderFrameStats.vsm_lod_bias).
typedef enum NphRenderVsmOverflow {
    NPH_RENDER_VSM_OVERFLOW_POOL = 1U << 0U,     // vsm_pages
    NPH_RENDER_VSM_OVERFLOW_MESHLETS = 1U << 1U, // max_vsm_meshlets
    NPH_RENDER_VSM_OVERFLOW_DRAWS = 1U << 2U,    // max_vsm_draws
} NphRenderVsmOverflow;

// The production path's lists that can fill up (NphRenderFrameStats.
// vis_overflow); each is a capacity in NphRendererDesc.
typedef enum NphRenderVisOverflow {
    NPH_RENDER_VIS_OVERFLOW_MESHLETS = 1U << 0U, // max_cull_meshlets
    NPH_RENDER_VIS_OVERFLOW_CLUSTERS = 1U << 1U, // max_visible_clusters
    NPH_RENDER_VIS_OVERFLOW_DEFERRED = 1U << 2U, // max_deferred
} NphRenderVisOverflow;

// Whether the renderer has the production path: the device has what it
// needs and the description did not disable it.
bool nph_renderer_production_supported(const NphRenderer *r);

// Whether the renderer traces shadow rays for lights that cast shadows:
// the device has ray queries and the description did not disable them.
bool nph_renderer_ray_tracing_supported(const NphRenderer *r);

// Whether the production path shadows its sun with a virtual shadow map:
// it has the production path, the device has depth clamping and clip
// distances, and the description did not disable it.
bool nph_renderer_virtual_shadows_supported(const NphRenderer *r);

// Declares this frame's passes into `graph` (between nph_gpu_rg_begin and
// nph_gpu_rg_compile) and fills the frame's GPU records. `target` is the
// image the tonemapped result is written to, typically the acquired
// swapchain image: its extent is the render extent, and it must be 2D with
// COLOR_ATTACHMENT usage in R8G8B8A8 or B8G8R8A8 (UNORM or sRGB) or
// A2B10G10R10_UNORM. The renderer imports it and returns its graph handle
// in `out_target` (may be NULL) for the caller's own passes and the
// present. Uploads queued since the last frame are flushed so the passes
// see them. Once per frame, inside a frame (nph_gpu_frame_begin), from the
// main thread; the passes record from the renderer's resources when the
// graph executes, so none may be created or destroyed in between.
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

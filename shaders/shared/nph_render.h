// SPDX-License-Identifier: Apache-2.0
// The renderer's GPU data (engine/render; docs/render.md), shared by its C
// code and its shaders (shaders/render/). Every record is reached through a
// buffer address; push constants carry the addresses and the indices.
//
// Conventions (ADR-0008, ADR-0018): positions on the GPU are camera
// relative (the camera sits at the origin, CLAUDE.md §8.2 "large-world
// coordinates"), lengths are metres, lights use the units of glTF's
// KHR_lights_punctual (lux for directional lights, candela for point and
// spot lights), emissive and environment radiance are cd/m^2, and the
// colour the forward pass writes is pre-exposed (multiplied by the view's
// exposure) so that sunlit specular highlights stay inside half-float
// range.
#ifndef NPH_RENDER_SHARED_H
#define NPH_RENDER_SHARED_H

#include "nph_shared.h"

// Light types (NphRenderLightData.direction.w).
#define NPH_RENDER_LIGHT_DIRECTIONAL 0
#define NPH_RENDER_LIGHT_POINT       1
#define NPH_RENDER_LIGHT_SPOT        2

// Material flags (NphRenderMaterialData.flags).
#define NPH_RENDER_MATERIAL_DOUBLE_SIDED       1
#define NPH_RENDER_MATERIAL_UNLIT              2
#define NPH_RENDER_MATERIAL_NORMAL_TWO_CHANNEL 4 // RG normal map: reconstruct z

// Material texture slots (NphRenderMaterialData.textures / samplers).
#define NPH_RENDER_TEX_BASE_COLOR         0
#define NPH_RENDER_TEX_METALLIC_ROUGHNESS 1 // glTF: roughness in G, metallic in B
#define NPH_RENDER_TEX_NORMAL             2
#define NPH_RENDER_TEX_OCCLUSION          3 // R
#define NPH_RENDER_TEX_EMISSIVE           4
#define NPH_RENDER_TEX_COUNT              5

// "No texture" in a material's texture slot.
#define NPH_RENDER_NO_TEXTURE 0xFFFFFFFFu

// Debug visualisations (NphRenderViewData.debug_mode).
#define NPH_RENDER_DEBUG_NONE       0
#define NPH_RENDER_DEBUG_BASE_COLOR 1
#define NPH_RENDER_DEBUG_NORMAL     2 // shading normal, camera-relative world space, as 0.5 + 0.5 n
#define NPH_RENDER_DEBUG_ROUGHNESS  3
#define NPH_RENDER_DEBUG_METALLIC   4
#define NPH_RENDER_DEBUG_OCCLUSION  5
#define NPH_RENDER_DEBUG_DIRECT     6  // punctual lights only
#define NPH_RENDER_DEBUG_INDIRECT   7  // image-based lighting only
#define NPH_RENDER_DEBUG_CLUSTERS   8  // the production path: a colour per visible cluster
#define NPH_RENDER_DEBUG_TRIANGLES  9  // the production path: a colour per triangle
#define NPH_RENDER_DEBUG_INSTANCES  10 // the production path: a colour per instance
#define NPH_RENDER_DEBUG_COUNT      11

// Tonemappers (NphRenderTonemapPush.tonemapper).
#define NPH_RENDER_TONEMAP_NONE  0 // clamp to [0, 1]
#define NPH_RENDER_TONEMAP_ACES  1 // Hill's fit of the ACES RRT + sRGB ODT
#define NPH_RENDER_TONEMAP_AGX   2 // Sobotka's AgX, base contrast, polynomial fit
#define NPH_RENDER_TONEMAP_COUNT 3

// View flags (NphRenderViewData.flags).
#define NPH_RENDER_VIEW_IBL 1 // the environment lights the scene

// Background modes (NphRenderViewData.background.w).
#define NPH_RENDER_BACKGROUND_ENVIRONMENT 0
#define NPH_RENDER_BACKGROUND_COLOR       1

// The largest finite half float: pre-exposed colour is clamped to it.
#define NPH_RENDER_HALF_MAX 65504.0

// Per-frame constants, one record per frame in flight.
NPH_SHARED_STRUCT(NphRenderViewData)
{
    NphMat4 view;          // camera-relative world -> view (rotation only)
    NphMat4 proj;          // view -> clip: reverse-Z, infinite far plane
    NphMat4 view_proj;     // camera-relative world -> clip
    NphMat4 inv_view_proj; // clip -> camera-relative world
    NphVec4 viewport;      // width, height, 1 / width, 1 / height
    NphVec4 debug_offset;  // xyz: the debug-draw origin relative to the camera
    NphVec4 background;    // rgb: solid background radiance (cd/m^2); w: NPH_RENDER_BACKGROUND_*
    u64 lights_address;    // NphRenderLightData[light_count]
    u64 materials_address; // NphRenderMaterialData[]
    u64 draws_address;     // NphRenderDrawData[]
    u64 sh_address;        // NphVec4[9]: cosine-convolved irradiance SH of the environment
    u64 debug_address;     // NphRenderDebugVertex[]
    u32 light_count;
    f32 exposure;         // radiance -> pre-exposed value
    f32 ibl_intensity;    // multiplier on the environment's radiance
    u32 specular_index;   // bindless: the prefiltered environment cube
    f32 specular_max_lod; // its mip count - 1 (lod = roughness * max_lod)
    u32 brdf_lut_index;   // bindless: the split-sum DFG table
    u32 env_index;        // bindless: the environment cube (background)
    u32 clamp_sampler;    // trilinear, clamp to edge
    u32 debug_mode;       // NPH_RENDER_DEBUG_*
    u32 flags;            // NPH_RENDER_VIEW_*
    u32 pad0;
    u32 pad1;
};

// One draw: a submesh of an instance. The transform is camera relative.
NPH_SHARED_STRUCT(NphRenderDrawData)
{
    NphVec4 model0; // rows of the affine transform: linear part in xyz, translation in w
    NphVec4 model1;
    NphVec4 model2;
    NphVec4 normal0; // rows of the inverse transpose of the linear part
    NphVec4 normal1;
    NphVec4 normal2;
    NphVec4 position_offset; // xyz: dequantise as offset + unorm16 * scale; w: unused
    NphVec4 position_scale;  // xyz; w: -1 when the transform mirrors (negative determinant), else 1
    u64 positions_address;   // NphRenderVertexPosition[] (4 x u16)
    u64 attribs_address;     // NphRenderVertexAttrib[] (normal, tangent, uv0, colour)
    u32 material_index;
    u32 instance_index;
    u32 pad0;
    u32 pad1;
};

// A metallic-roughness material (glTF 2.0).
NPH_SHARED_STRUCT(NphRenderMaterialData)
{
    NphVec4 base_color; // linear RGBA factor
    NphVec4 emissive;   // rgb: radiance (cd/m^2) = factor * strength; w: unused
    f32 metallic;
    f32 roughness; // perceptual roughness
    f32 alpha_cutoff;
    f32 normal_scale;
    f32 occlusion_strength;
    u32 flags;       // NPH_RENDER_MATERIAL_*
    u32 alpha_mode;  // 0 opaque, 1 mask, 2 blend
    u32 handle;      // the material's handle value (0: the default material, or a free slot)
    u32 textures[5]; // bindless sampled-image indices (NPH_RENDER_NO_TEXTURE: none)
    u32 samplers[5]; // bindless sampler indices
    u32 pad1;
    u32 pad2;
};

// A punctual light (KHR_lights_punctual), camera relative.
NPH_SHARED_STRUCT(NphRenderLightData)
{
    NphVec4 position;  // xyz: position; w: 1 / range^2 (0: unlimited range)
    NphVec4 direction; // xyz: the direction the light travels (unit); w: NPH_RENDER_LIGHT_*
    NphVec4 color;     // rgb: colour * intensity (lux or candela); w: unused
    NphVec4 spot;      // x: angle scale, y: angle offset (spot lights); zw: unused
};

// A debug line end point.
NPH_SHARED_STRUCT(NphRenderDebugVertex)
{
    f32 x; // relative to the debug-draw origin
    f32 y;
    f32 z;
    u32 color; // RGBA8, sRGB-encoded like an authored colour
};

// The procedural sky that seeds an environment (ibl_sky.comp and the C
// reference in render/reference.h).
NPH_SHARED_STRUCT(NphRenderSkyData)
{
    NphVec4 zenith;  // rgb radiance (cd/m^2) straight up
    NphVec4 horizon; // rgb radiance at the horizon
    NphVec4 ground;  // rgb radiance of the ground (below the horizon)
    NphVec4 params;  // x: zenith exponent (sky = mix(horizon, zenith, h^x)); y: ground blend
                     // sharpness; zw: unused
};

// NphRenderPush.flags
#define NPH_RENDER_PUSH_LINEAR_OUTPUT 1 // debug lines: the target is sRGB, write linear colour

// Push constants of the draw shaders (forward, sky, debug lines).
NPH_SHARED_STRUCT(NphRenderPush)
{
    u64 view_address;    // NphRenderViewData
    u32 draw_index;      // forward: the record in draws
    u32 flags;           // NPH_RENDER_PUSH_*
    u32 visbuffer_index; // the production path's debug lines: the visibility buffer
    u32 pad0;
};

// Push constants of the tonemap pass.
NPH_SHARED_STRUCT(NphRenderTonemapPush)
{
    u32 hdr_index;   // bindless: the pre-exposed HDR colour
    u32 tonemapper;  // NPH_RENDER_TONEMAP_*
    u32 encode_srgb; // 1: the target is UNORM, so the shader applies the sRGB curve
    u32 pad0;
};

// Push constants of the image-based lighting kernels.
NPH_SHARED_STRUCT(NphRenderIblPush)
{
    NphVec4 params;    // kernel-specific (see each shader)
    u64 address;       // ibl_sky: NphRenderSkyData; ibl_sh: NphVec4[9] output
    u32 src_index;     // bindless source (sampled or storage, per kernel)
    u32 dst_index;     // bindless storage destination (one mip)
    u32 sampler_index; // for sampled sources
    u32 size;          // destination extent (square faces)
    u32 src_size;      // source extent (level 0)
    u32 sample_count;  // ibl_prefilter, brdf_lut
};

#endif

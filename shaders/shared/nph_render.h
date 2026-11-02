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
#define NPH_RENDER_DEBUG_IRRADIANCE 8  // the diffuse light's irradiance, over pi
#define NPH_RENDER_DEBUG_CLUSTERS   9  // the production path: a colour per visible cluster
#define NPH_RENDER_DEBUG_TRIANGLES  10 // the production path: a colour per triangle
#define NPH_RENDER_DEBUG_INSTANCES  11 // the production path: a colour per instance
#define NPH_RENDER_DEBUG_COUNT      12

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

// The production path's clustered lights (NphRenderLightGrid).
#define NPH_RENDER_LIGHT_TILE   16   // pixels: the screen tiles of the light masks
#define NPH_RENDER_ZBINS        1024 // view-distance bins
#define NPH_RENDER_ZBIN_OCTAVES 20   // the bins span znear to znear * 2^20, evenly in log2
#define NPH_RENDER_MAX_LIGHTS   4096 // a view's lights (16-bit indices in the bins)

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
    // The production path's NphRenderLightGrid; 0: every light of
    // lights_address is evaluated for every pixel (the reference).
    u64 light_grid;
    // The top-level acceleration structure of the instances, for the shaders
    // with ray queries (0: none this frame). Its space is the camera's
    // shifted by tlas_offset: a point p of the view is p + tlas_offset there.
    u64 tlas;
    NphVec4 tlas_offset; // xyz: the camera's position in the top level's space; w: unused
    // The production path's virtual shadow map this frame (NphVsmView, nph_vsm.h;
    // 0: none), which the sun's light record (color.w 2) is shadowed by.
    u64 vsm;
    // The production path's radiance cache this frame (NphRenderGiView; 0:
    // none), whose irradiance the diffuse indirect light takes.
    u64 gi;
    // xy: the sub-pixel offset of this frame's projection, in NDC (zero
    // without temporal anti-aliasing), which what is drawn over the temporal
    // resolve's output takes off again; zw: unused.
    NphVec4 jitter;
    // The frame's paged textures (NphRenderPagingView; 0: none paged).
    u64 paging;
    // The many lights this frame (NphRenderManyView; 0: none).
    u64 many;
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
    // Bindless sampler indices: a paged texture's clamps the level of detail
    // to its first resident level this frame (texture_paging.glsl).
    u32 samplers[5];
    // How each slot's sampler addresses: bits 4s..4s+1 its u, 4s+2..4s+3 its
    // v (NPH_RENDER_WRAP_*), which paged textures' requests follow.
    u32 wrap;
    u32 pad2;
};

// Paged textures (docs/adr/0065-virtual-texturing.md; texture_paging.glsl):
// the levels above a texture's mip tail made resident whole. A paged
// texture's record, indexed by its bindless sampled-image index, says
// where its request words start and how its pages are cut; a record of
// zero is a texture resident whole.
NPH_SHARED_STRUCT(NphRenderTexturePaging)
{
    // Its request words: the first its own, the others a bit per page,
    // level by level, row by row.
    u32 first_word;
    u32 extent; // level 0's width | height << 16
    // log2 of the page width | log2 of its height << 4 | the first level
    // resident from creation << 8 (levels from there are always resident:
    // the mip tail, or the levels under a page); 0: not paged.
    u32 shape;
    u32 pad;
    // The first bit of each paged level among the texture's bits (after its
    // first word), two levels a field: level 2i in the low half, 2i + 1 in
    // the high. Fields, not an array: shaders copy the record
    // (docs/build.md "Shaders").
    u32 levels01;
    u32 levels23;
    u32 levels45;
    u32 levels67;
};

// The frame's paged textures (NphRenderViewData.paging).
NPH_SHARED_STRUCT(NphRenderPagingView)
{
    u64 records;        // NphRenderTexturePaging[], indexed by bindless sampled-image index
    u64 requests;       // u32 words: the pages this frame's pixels asked for
    f32 max_anisotropy; // the material samplers'
    u32 feedback_phase; // the pixel of each 8 x 8 tile that asks this frame: x | y << 3
};

// A punctual light (KHR_lights_punctual), camera relative.
NPH_SHARED_STRUCT(NphRenderLightData)
{
    NphVec4 position;  // xyz: position; w: 1 / range^2 (0: unlimited range)
    NphVec4 direction; // xyz: the direction the light travels (unit); w: NPH_RENDER_LIGHT_*
    NphVec4 color;     // rgb: colour * intensity (lux or candela); w: 1 when it casts shadows
                       // (shadow rays), 2 for the production path's sun (its virtual shadow
                       // map where the view has one, else shadow rays)
    NphVec4 spot;      // spot lights: x, y the angle scale and offset; z, w the cosine and sine
                       // of the outer cone angle (culling)
};

// The lights of a production frame, binned for shading (docs/render.md
// "Lights"; Drobot, "Improved Culling for Tiled and Clustered Rendering",
// SIGGRAPH 2017). Lights that reach every pixel (directional lights and
// those of unlimited range) are listed apart; the others in view are
// sorted by the bin of the nearest distance they reach. A pixel evaluates
// the sorted lights its distance bin may meet that its screen tile's mask
// also holds: the bins and the tiles each bound one dimension, so the
// grid costs a word per bin and a bit per light and tile instead of a list
// per 3D cell (Olsson et al., "Clustered Deferred and Forward Shading",
// HPG 2012). Built on the GPU each frame (vis_lights.comp,
// vis_light_bins.comp, vis_light_tiles.comp).
NPH_SHARED_STRUCT(NphRenderLightGrid)
{
    u64 globals;       // NphRenderLightData[]: the lights that reach every pixel
    u64 locals;        // NphRenderLightData[]: the others in view, by nearest distance's bin
    u64 zbins;         // u32[NPH_RENDER_ZBINS]: first | last << 16, the range of locals that may
                       // reach the bin (first > last: none)
    u64 tiles;         // u32[word][tile]: bit b of word w set when local 32 w + b may reach the
                       // tile's opaque surfaces (the visibility buffer's)
    u64 tiles_blended; // u32[word][tile]: the same for blended surfaces, which lie anywhere in
                       // front of them (or of the background)
    u64 counts;        // u32[2]: the global and the local lights' counts (written on the GPU)
    f32 znear;         // the projection's near plane
    f32 zbin_scale;    // bins per doubling of the view distance
    u32 tiles_x;       // tiles per row (NPH_RENDER_LIGHT_TILE pixels square)
    u32 tile_count;    // tiles in all: the stride from one word of a tile to its next
};

// One of the many lights (docs/render.md "Many lights"; many_lights.c), in world
// space about its set's origin.
NPH_SHARED_STRUCT(NphRenderManyLightData)
{
    NphVec4 position; // xyz: about NphRenderManyView's origin; w: 1 / near_range^2
    NphVec4 color;    // rgb: colour * intensity (candela); w: 1 / range^2 (0: unlimited)
};

// A table entry: a world cell's lights, the first in `indices` and how many. Cells
// hash to their entries, and two cells may share one: a pixel tests each light's
// distance anyway.
#define NPH_RENDER_MANY_COUNT_SHIFT   22
#define NPH_RENDER_MANY_FIRST_MASK    0x3FFFFF
#define NPH_RENDER_MANY_HASH(x, y, z) (((x) * 73856093u) ^ ((y) * 19349663u) ^ ((z) * 83492791u))

// The many lights of a frame.
NPH_SHARED_STRUCT(NphRenderManyView)
{
    u64 records; // NphRenderManyLightData[count]
    u64 nodes;   // the light tree's nodes, six NphVec4s each (render/light_tree.h)
    u64 table;   // u32[table_mask + 1]: first | count << NPH_RENDER_MANY_COUNT_SHIFT
    u64 indices; // u32[]: the entries' lists, indices into `records`
    // xyz: what takes a camera-relative point about the set's origin; w: cells a metre.
    NphVec4 to_origin;
    u32 table_mask;
    u32 count;
    f32 near2; // the greatest near range, squared
    u32 pad;
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
    u64 view_address; // NphRenderViewData
    u32 draw_index;   // forward: the record in draws
    u32 flags;        // NPH_RENDER_PUSH_*
    // The production path's visibility buffer, bindless sampled (R32G32_UINT:
    // the depth's bits in y), whose depth the blended surfaces, their page
    // requests, and the depth-tested debug lines test themselves
    u32 visbuffer_index;
    u32 pad0;
};

// Push constants of the tonemap pass.
NPH_SHARED_STRUCT(NphRenderTonemapPush)
{
    u32 hdr_index;     // bindless: the pre-exposed HDR colour
    u32 tonemapper;    // NPH_RENDER_TONEMAP_*
    u32 encode_srgb;   // 1: the target is UNORM, so the shader applies the sRGB curve
    u32 bloom_index;   // bindless sampled: the bloom chain, whose mip 0 sums its levels
    f32 bloom;         // the bloom's weight in the image (0: none)
    f32 bloom_scale;   // 1 / the chain's levels: mip 0 back to one blur's energy
    u32 sampler_index; // bilinear, clamped to the edge
    u32 pad0;
    NphVec2 bloom_uv; // a pixel's step in mip 0's UV: 0.5 / its size (it halves the frame)
};

// Bloom and exposure (post.c; bloom.comp, exposure.comp).
#define NPH_RENDER_BLOOM_LEVELS 6 // the chain's mips at most, the first half the frame's size
// The bloom and metering kernels (bloom.comp's specialisation, or
// exposure.comp).
#define NPH_RENDER_POST_DOWN_FIRST 0 // the HDR image to the chain's mip 0 (Karis average)
#define NPH_RENDER_POST_DOWN       1 // mip k - 1 to mip k
#define NPH_RENDER_POST_UP         2 // mip k += the tent of mip k + 1
#define NPH_RENDER_POST_AVERAGE    3 // the histogram's mean between its percentiles
// The exposure histogram: log2 of pre-exposed luminance over
// [MIN_LOG2, MIN_LOG2 + RANGE_LOG2) in BINS bins (an eighth of a stop
// each); the metered mean takes the pixels between the LOW and HIGH
// fractions of the sorted luminances.
#define NPH_RENDER_EXPOSURE_BINS       128
#define NPH_RENDER_EXPOSURE_MIN_LOG2   (-12.0)
#define NPH_RENDER_EXPOSURE_RANGE_LOG2 16.0
#define NPH_RENDER_EXPOSURE_LOW        0.5
#define NPH_RENDER_EXPOSURE_HIGH       0.95

// The frame's metering, on the GPU (cleared each frame; read back).
NPH_SHARED_STRUCT(NphRenderExposureData)
{
    u32 histogram[NPH_RENDER_EXPOSURE_BINS];
    f32 average_log2; // mean log2 pre-exposed luminance between the percentiles
    u32 count;        // the pixels counted
    u32 pad0;
    u32 pad1;
};

// Push constants of the bloom and exposure kernels.
NPH_SHARED_STRUCT(NphRenderPostPush)
{
    u64 exposure;      // NphRenderExposureData; 0: no metering
    NphVec2 src_texel; // the source mip's texel, in UV
    u32 src_index;     // bindless sampled: the HDR image, or the chain
    f32 src_lod;       // the source's mip
    u32 dst_index;     // bindless storage: the destination mip
    u32 sampler_index; // bilinear, clamped to the edge
    u32 dst_width;
    u32 dst_height;
    u32 noise; // the rounding noise's offset this frame
    u32 pad0;
};

// Push constants of the production path's temporal resolve (taa.frag).
NPH_SHARED_STRUCT(NphRenderTaaPush)
{
    u64 view; // NphVisViewData
    // The reconstruction filter's weights of the 3 x 3 samples, row by row
    // from the top left (they sum to 1), for this frame's jitter.
    NphVec4 weights0;
    NphVec4 weights1;
    f32 weight8;
    f32 history_scale;   // this frame's exposure over the history's; 0: no history
    NphVec2 jitter;      // this frame's sub-pixel offset of the projection, in UV
    u32 hdr_index;       // bindless sampled: the pre-exposed HDR colour
    u32 visbuffer_index; // bindless sampled: the visibility buffer as R32G32_UINT (depth in y)
    u32 history_index;   // bindless sampled: the previous frame's history
    u32 sampler_index;   // bilinear, clamped to the edge: the history's taps
    u32 point_sampler;   // nearest, clamped to the edge: the visibility buffer's gathers
    u32 tonemapper;      // NPH_RENDER_TONEMAP_*
    u32 encode_srgb;     // 1: the target is UNORM, so the shader applies the sRGB curve
    u32 noise;           // the history's rounding noise's offset this frame
    u32 bloom_index;     // as NphRenderTonemapPush's
    f32 bloom;
    f32 bloom_scale;
    u32 pad0;
    NphVec2 bloom_uv;
};

// Motion blur (motion_blur.c; motion.comp, motion_tiles.comp, motion_blur.frag;
// docs/render.md "Motion blur"; Guertin, McGuire & Nowrouzezahrai, "A Fast and
// Stable Feature-Aware Motion Blur Filter", HPG 2014).
#define NPH_RENDER_MB_TILE 32 // pixels: the dominant motion's tiles; no blur reaches farther
// The gather's samples, a pixel apart along the dominant blur (two per
// pixel of its half-extent, or four where they alternate between two
// directions), within these bounds: where the neighbourhood moves as one,
// the colour's bilinear taps alone, at most UNIFORM_SAMPLES; elsewhere a
// colour and a motion each, at most MAX_SAMPLES.
#define NPH_RENDER_MB_MIN_SAMPLES     4
#define NPH_RENDER_MB_UNIFORM_SAMPLES 16
#define NPH_RENDER_MB_MAX_SAMPLES     32

// Push constants of the motion and tile kernels (motion.comp, motion_tiles.comp).
NPH_SHARED_STRUCT(NphRenderMotionPush)
{
    u64 view;             // NphVisViewData
    f32 scale;            // a pixel of motion's blur half-extent: 0.5 * shutter / frame_seconds
    u32 visbuffer_index;  // bindless sampled: the visibility buffer as R32G32_UINT (depth in y)
    u32 motion_index;     // bindless storage: the motion image (blur half-extent xy, distance z)
    u32 tiles_index;      // bindless storage: each tile's dominant blur
    u32 neighbours_index; // bindless storage: each tile's 3 x 3 neighbourhood's
    u32 tiles_x;          // tiles per row
    u32 tiles_y;          // and rows
    u32 point_sampler;    // nearest, clamped to the edge: the visibility buffer's gathers
};

// Push constants of the motion blur's gather, fused with the tonemap
// (motion_blur.frag).
NPH_SHARED_STRUCT(NphRenderMotionBlurPush)
{
    u32 color_index;      // bindless sampled: the pre-exposed colour (the temporal history, or
                          // the HDR image)
    u32 motion_index;     // bindless sampled: the motion image
    u32 neighbours_index; // bindless sampled: the neighbourhoods' dominant blur
    u32 tonemapper;       // NPH_RENDER_TONEMAP_*
    u32 encode_srgb;      // 1: the target is UNORM, so the shader applies the sRGB curve
    u32 noise;            // the samples' jitter's offset this frame
    u32 bloom_index;      // as NphRenderTonemapPush's (bloom 0 when the history carries it)
    f32 bloom;
    f32 bloom_scale;
    u32 sampler_index; // bilinear, clamped to the edge: the glare, and the colour's taps where
                       // the blur is uniform
    u32 width;         // the target's extent
    u32 height;
    NphVec2 bloom_uv;
    NphVec2 inv_size; // 1 / the extent
};

// Depth of field (dof.c; dof_setup.comp, dof_tiles.comp, dof_gather.comp,
// dof_resolve.comp; docs/render.md "Depth of field").
#define NPH_RENDER_DOF_TILE 32 // full-resolution pixels: the circles' tiles
// The largest circle of confusion's radius, in full-resolution pixels: a
// tile, so that no circle reaches past the tiles around its own.
#define NPH_RENDER_DOF_MAX_RADIUS 32.0
// Circles of a radius under this many full-resolution pixels are sharp.
#define NPH_RENDER_DOF_SHARP 0.5
// Tiles no circle of a radius over this many full-resolution pixels can
// reach are gathered at full resolution (dof_resolve.comp), the rest at
// half (dof_gather.comp).
#define NPH_RENDER_DOF_SLIGHT 8.0

// Push constants of the depth of field's kernels.
NPH_SHARED_STRUCT(NphRenderDofPush)
{
    // A pixel's circle of confusion's radius in full-resolution pixels at
    // reverse-Z depth d: bias + scale * d (a thin lens; negative in front of
    // the focus), within NPH_RENDER_DOF_MAX_RADIUS.
    f32 coc_bias;
    f32 coc_scale;
    u32 depth_index;      // bindless sampled: the visibility buffer as R32G32_UINT (depth in
                          // y; the production path), or the depth image (the reference)
    u32 color_index;      // bindless sampled: the HDR image as drawn (setup, resolve)
    u32 half_index;       // the half-resolution colour and its block's nearest depth: storage
                          // (setup), sampled
    u32 full_depth_index; // the full-resolution depths: storage (setup), sampled
    u32 tiles_index;      // bindless storage: each tile's nearest and farthest circles
    u32 neighbours_index; // their 3 x 3 neighbourhoods', and the largest circle within two
                          // tiles: storage (tiles), sampled
    u32 far_index;        // the far layer: storage (gather), sampled (resolve)
    u32 near_index;       // the near layer: storage (gather), sampled (resolve)
    u32 out_index;        // bindless storage: the HDR image with depth of field (resolve)
    u32 point_sampler;    // nearest, clamped to the edge
    u32 linear_sampler;   // bilinear, clamped to the edge
    u32 width;            // the full-resolution extent
    u32 height;
    u32 tiles_x;      // tiles per row
    u32 tiles_y;      // and rows
    u32 noise;        // the samples' rotation's offset this frame
    NphVec2 inv_size; // 1 / the full-resolution extent
};

// Ambient occlusion (ao.c; ao_depth.comp, ao.comp, ao_denoise.comp, and the
// resolve's read; docs/render.md "Ambient occlusion"), at full resolution:
// one occlusion a pixel.
// The most pixels a search for horizons reaches, as a fraction of the
// image's height: nearer surfaces search a radius the view shrinks.
#define NPH_RENDER_AO_MAX_RADIUS 0.25
// Distances at or beyond this many metres are the sky's.
#define NPH_RENDER_AO_SKY 1.0e8
// The depths' mips (ao_depth.comp), each the average of the reverse-Z depths
// below it: exact where they lie on a plane. Mip 0 is the frame's.
#define NPH_RENDER_AO_MIPS 6

// Push constants of the ambient occlusion's kernels.
NPH_SHARED_STRUCT(NphRenderAoPush)
{
    // A point's view-space position at view distance d under the pixel
    // position c (full-resolution pixels): d * (unproject.xy * c +
    // unproject.zw, -1), through this frame's projection, jitter included.
    NphVec4 unproject;
    NphVec2 project;     // 1 / unproject.xy
    NphVec2 depth_texel; // a texel of the depths' mip 0 in uv: 1 / their extent
    f32 znear;           // the near plane (reverse-Z depth d is znear / distance)
    f32 radius;          // the search's radius in metres
    f32 radius_pixels;   // the radius in full-resolution pixels at a distance of one metre
    f32 max_radius;      // the most full-resolution pixels a search reaches
    u32 depths_index;    // bindless sampled: the reverse-Z depths, every mip
                         // (the extent rounded up to a multiple of 2^(NPH_RENDER_AO_MIPS - 1))
    u32 depth_mips[NPH_RENDER_AO_MIPS]; // bindless storage: each of their mips (depth)
    u32 raw_index; // bindless: each pixel's occlusion, storage (ao), sampled (denoise)
    u32 out_index; // bindless storage: the denoised occlusion (denoise)
    u32 width;     // the frame's extent
    u32 height;
    u32 noise;          // the slices' rotation this frame (0 to 5)
    u32 point_sampler;  // nearest, clamped to the edge
    u32 linear_sampler; // bilinear, clamped to the edge
};

// ---------------------------------------------------------------------------
// The radiance cache (gi.c, gi_common.glsl; docs/render.md "Global
// illumination")
// ---------------------------------------------------------------------------

// Its cells' levels: level l holds the points 2^(l - NPH_RENDER_GI_LEVEL_BIAS)
// to twice that far from the camera, in cells 2^(l - NPH_RENDER_GI_LEVEL_BIAS)
// / NphRenderGiView.scale metres across (level 8: 1 to 2 m away).
#define NPH_RENDER_GI_LEVELS     32
#define NPH_RENDER_GI_LEVEL_BIAS 8
// The slots a key may take: the NPH_RENDER_GI_BUCKET of its bucket.
#define NPH_RENDER_GI_BUCKET 16
// A cell's history holds at most this many samples (its window).
#define NPH_RENDER_GI_HISTORY 128
// A cell no sample has reached for this many frames is freed.
#define NPH_RENDER_GI_MAX_AGE 240
// The most a sample may add, in pre-exposed irradiance: brighter ones are
// clamped (fireflies; the sums are fixed point).
#define NPH_RENDER_GI_SAMPLE_MAX 4096.0

// Where the camera is at one level: the cell it is in, and where in it.
NPH_SHARED_STRUCT(NphRenderGiLevel)
{
    NphVec4 origin; // xyz: the camera's offset from its cell's corner (m, [0, size)); w: size
    // xyz: the camera's cell, its two's complement's low 32 bits (the keys
    // keep 17); w: unused
    u32 cell[4];
};

// A production frame's radiance cache (NphRenderViewData.gi): a hash grid of
// cells, each the irradiance of the surfaces in it facing one octant.
NPH_SHARED_STRUCT(NphRenderGiView)
{
    u64 keys;  // u32[capacity]: each slot's key fingerprint (0: free)
    u64 sums;  // uvec4[capacity]: this frame's samples (rgb in 1 / sum_scale, and their count)
    u64 cells; // uvec4[capacity]: irradiance (rgb, lux, as a float's bits); w: the sample
               // count in bits 0-15 and the frames since one arrived in bits 16-31
    u32 bucket_mask; // capacity / NPH_RENDER_GI_BUCKET - 1
    f32 scale;       // a cell is 1 / (2 scale) to 1 / scale of its distance across
    f32 sum_scale;   // fixed-point units of a pre-exposed sample
    f32 near;        // update rays that hit nearer count nothing (the ambient occlusion's)
    u32 frame;       // this frame's number, for the random numbers
    u32 jitter;      // nonzero: a lookup takes a random point of its cell (temporal
                     // anti-aliasing averages them)
    u32 rays_x;      // the update's samples across the target
    u32 stride;      // a sample a stride x stride block of pixels
    u32 pad0;
    u32 pad1;
    // xyz: the direction the sun's light travels; w: 1 when it casts
    // shadows, 0 when it does not, -1 when there is no sun
    NphVec4 sun_direction;
    NphVec4 sun_color; // rgb: its illuminance times its colour (lux)
    NphRenderGiLevel levels[NPH_RENDER_GI_LEVELS];
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

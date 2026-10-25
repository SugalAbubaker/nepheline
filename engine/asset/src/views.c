// SPDX-License-Identifier: Apache-2.0
// Typed views over loaded assets (mesh.h, texture.h, material.h, scene.h,
// sound.h): each resolves the blobs an asset type defines and checks every
// count and range, so the renderer, the scene code, and the audio runtime
// can use the pointers without further validation.
#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/sound.h>
#include <nepheline/asset/texture.h>
#include <nepheline/core/assert.h>

#include <string.h>

// Finds the blob and checks its size against `count` elements of
// `elem_size` bytes. A missing blob is allowed when count is zero (the
// pointer is then NULL).
static NphResult typed_blob(const NphAsset *asset, u32 tag, u32 index, u64 elem_size, u64 count,
                            const void **out)
{
    const NphAssetBlob *b = nph_asset_blob(asset, tag, index);
    if (b == NULL) {
        *out = NULL;
        return count == 0U ? NPH_RESULT_OK : NPH_RESULT_CORRUPT;
    }
    if (count > NPH_U64_MAX / elem_size || b->size != elem_size * count) {
        return NPH_RESULT_CORRUPT;
    }
    *out = b->data;
    return NPH_RESULT_OK;
}

static NphResult check_type(const NphAsset *asset, u32 type, u32 version)
{
    NPH_ASSERT(asset != NULL);
    if (asset->header.type != type) {
        return NPH_RESULT_TYPE_MISMATCH;
    }
    if (asset->header.type_version != version) {
        return NPH_RESULT_VERSION_MISMATCH;
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Mesh
// ---------------------------------------------------------------------------

NphResult nph_mesh_view(const NphAsset *asset, NphMeshView *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    NPH_TRY(check_type(asset, NPH_ASSET_TYPE_MESH, NPH_MESH_TYPE_VERSION));
    const void *p = NULL;
    NPH_TRY(typed_blob(asset, NPH_MESH_TAG_HEADER, 0U, sizeof(NphMeshHeader), 1U, &p));
    const NphMeshHeader *h = (const NphMeshHeader *)p;
    if (h->index_count % 3U != 0U || h->reserved0 != 0U) {
        return NPH_RESULT_CORRUPT;
    }
    NPH_TRY(typed_blob(asset, NPH_MESH_TAG_POSITIONS, 0U, sizeof(NphMeshPosition), h->vertex_count,
                       &p));
    const NphMeshPosition *positions = (const NphMeshPosition *)p;
    NPH_TRY(
        typed_blob(asset, NPH_MESH_TAG_ATTRIBS, 0U, sizeof(NphMeshAttrib), h->vertex_count, &p));
    const NphMeshAttrib *attribs = (const NphMeshAttrib *)p;
    NPH_TRY(typed_blob(asset, NPH_MESH_TAG_INDICES, 0U, sizeof(u32), h->index_count, &p));
    const u32 *indices = (const u32 *)p;
    NPH_TRY(
        typed_blob(asset, NPH_MESH_TAG_SUBMESHES, 0U, sizeof(NphSubmesh), h->submesh_count, &p));
    const NphSubmesh *submeshes = (const NphSubmesh *)p;
    NPH_TRY(typed_blob(asset, NPH_MESH_TAG_MESHLETS, 0U, sizeof(NphMeshlet), h->meshlet_count, &p));
    const NphMeshlet *meshlets = (const NphMeshlet *)p;
    NPH_TRY(typed_blob(asset, NPH_MESH_TAG_MESHLET_BOUNDS, 0U, sizeof(NphMeshletBounds),
                       h->meshlet_count, &p));
    const NphMeshletBounds *bounds = (const NphMeshletBounds *)p;
    NPH_TRY(typed_blob(asset, NPH_MESH_TAG_MESHLET_VERTICES, 0U, sizeof(u32),
                       h->meshlet_vertex_count, &p));
    const u32 *meshlet_vertices = (const u32 *)p;
    NPH_TRY(typed_blob(asset, NPH_MESH_TAG_MESHLET_TRIS, 0U, 3U, h->meshlet_triangle_count, &p));
    const u8 *meshlet_triangles = (const u8 *)p;

    for (u32 i = 0; i < h->index_count; ++i) {
        if (indices[i] >= h->vertex_count) {
            return NPH_RESULT_CORRUPT;
        }
    }
    for (u32 i = 0; i < h->submesh_count; ++i) {
        const NphSubmesh *s = &submeshes[i];
        if (s->index_count % 3U != 0U || s->index_offset > h->index_count ||
            s->index_count > h->index_count - s->index_offset ||
            s->meshlet_offset > h->meshlet_count ||
            s->meshlet_count > h->meshlet_count - s->meshlet_offset) {
            return NPH_RESULT_CORRUPT;
        }
    }
    for (u32 i = 0; i < h->meshlet_count; ++i) {
        const NphMeshlet *m = &meshlets[i];
        if (m->vertex_count > NPH_MESHLET_MAX_VERTICES ||
            m->triangle_count > NPH_MESHLET_MAX_TRIANGLES || m->submesh >= h->submesh_count ||
            m->vertex_offset > h->meshlet_vertex_count ||
            m->vertex_count > h->meshlet_vertex_count - m->vertex_offset ||
            m->triangle_offset > h->meshlet_triangle_count ||
            m->triangle_count > h->meshlet_triangle_count - m->triangle_offset) {
            return NPH_RESULT_CORRUPT;
        }
        for (u32 v = 0; v < m->vertex_count; ++v) {
            if (meshlet_vertices[m->vertex_offset + v] >= h->vertex_count) {
                return NPH_RESULT_CORRUPT;
            }
        }
        if (m->triangle_count == 0U) {
            // Nothing to check, and no pointer to form: with no meshlet
            // triangles in the mesh their blob is absent (NULL), and C has no
            // arithmetic on a null pointer, not even by zero.
            continue;
        }
        const u8 *tris = meshlet_triangles + (u64)m->triangle_offset * 3U;
        for (u32 t = 0; t < (u32)m->triangle_count * 3U; ++t) {
            if (tris[t] >= m->vertex_count) {
                return NPH_RESULT_CORRUPT;
            }
        }
    }
    out->header = h;
    out->positions = positions;
    out->attribs = attribs;
    out->indices = indices;
    out->submeshes = submeshes;
    out->meshlets = meshlets;
    out->meshlet_bounds = bounds;
    out->meshlet_vertices = meshlet_vertices;
    out->meshlet_triangles = meshlet_triangles;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Texture
// ---------------------------------------------------------------------------

static const NphTextureFormatInfo g_texture_formats[] = {
    {"undefined", 0U, 1U, 0U, 0, 0},           // 0
    {"R8_UNORM", 1U, 1U, 1U, 0, 0},            // 1
    {"R8G8_UNORM", 2U, 1U, 2U, 0, 0},          // 2
    {"R8G8B8A8_UNORM", 4U, 1U, 4U, 0, 0},      // 3
    {"R8G8B8A8_SRGB", 4U, 1U, 4U, 1, 0},       // 4
    {"R16G16B16A16_SFLOAT", 8U, 1U, 4U, 0, 0}, // 5
    {"undefined", 0U, 1U, 0U, 0, 0},           // 6
    {"undefined", 0U, 1U, 0U, 0, 0},           // 7
    {"undefined", 0U, 1U, 0U, 0, 0},           // 8
    {"undefined", 0U, 1U, 0U, 0, 0},           // 9
    {"BC4_UNORM", 8U, 4U, 1U, 0, 1},           // 10
    {"BC5_UNORM", 16U, 4U, 2U, 0, 1},          // 11
    {"BC6H_UFLOAT", 16U, 4U, 3U, 0, 1},        // 12
    {"BC7_UNORM", 16U, 4U, 4U, 0, 1},          // 13
    {"BC7_SRGB", 16U, 4U, 4U, 1, 1},           // 14
};

const NphTextureFormatInfo *nph_texture_format_info(u32 format)
{
    if (format >= NPH_ARRAY_COUNT(g_texture_formats)) {
        return &g_texture_formats[0];
    }
    return &g_texture_formats[format];
}

u64 nph_texture_level_bytes(u32 format, u32 width, u32 height)
{
    const NphTextureFormatInfo *info = nph_texture_format_info(format);
    if (info->bytes_per_block == 0U) {
        return 0U;
    }
    const u64 bw = ((u64)width + info->block_size - 1U) / info->block_size;
    const u64 bh = ((u64)height + info->block_size - 1U) / info->block_size;
    return bw * bh * info->bytes_per_block;
}

u32 nph_texture_mip_count(u32 width, u32 height)
{
    const u32 e = nph_max_u32(nph_max_u32(width, height), 1U);
    return nph_log2_floor_u32(e) + 1U;
}

NphResult nph_texture_view(const NphAsset *asset, NphTextureView *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    NPH_TRY(check_type(asset, NPH_ASSET_TYPE_TEXTURE, NPH_TEXTURE_TYPE_VERSION));
    const void *p = NULL;
    NPH_TRY(typed_blob(asset, NPH_TEXTURE_TAG_HEADER, 0U, sizeof(NphTextureHeader), 1U, &p));
    const NphTextureHeader *h = (const NphTextureHeader *)p;
    if (h->width == 0U || h->width > NPH_TEXTURE_MAX_EXTENT || h->height == 0U ||
        h->height > NPH_TEXTURE_MAX_EXTENT || h->mip_count == 0U ||
        h->mip_count > nph_texture_mip_count(h->width, h->height) || h->layer_count == 0U ||
        h->layer_count > NPH_TEXTURE_MAX_LAYERS ||
        nph_texture_format_info(h->format)->bytes_per_block == 0U ||
        h->kind > (u32)NPH_TEXTURE_CUBE || (h->kind == NPH_TEXTURE_2D && h->layer_count != 1U) ||
        (h->kind == NPH_TEXTURE_CUBE && h->layer_count % 6U != 0U)) {
        return NPH_RESULT_CORRUPT;
    }
    for (u32 i = 0; i < NPH_ARRAY_COUNT(h->reserved); ++i) {
        if (h->reserved[i] != 0U) {
            return NPH_RESULT_CORRUPT;
        }
    }
    for (u32 m = 0; m < h->mip_count; ++m) {
        const u64 level = nph_texture_level_bytes(h->format, nph_texture_mip_extent(h->width, m),
                                                  nph_texture_mip_extent(h->height, m));
        const u64 bytes = level * h->layer_count;
        NPH_TRY(typed_blob(asset, NPH_TEXTURE_TAG_MIP, m, 1U, bytes, &p));
        out->mips[m] = p;
        out->mip_sizes[m] = bytes;
    }
    out->header = h;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Material
// ---------------------------------------------------------------------------

NphResult nph_material_view(const NphAsset *asset, NphMaterialView *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    NPH_TRY(check_type(asset, NPH_ASSET_TYPE_MATERIAL, NPH_MATERIAL_TYPE_VERSION));
    const void *p = NULL;
    NPH_TRY(typed_blob(asset, NPH_MATERIAL_TAG_HEADER, 0U, sizeof(NphMaterialHeader), 1U, &p));
    const NphMaterialHeader *h = (const NphMaterialHeader *)p;
    if (h->alpha_mode > (u32)NPH_MATERIAL_ALPHA_BLEND) {
        return NPH_RESULT_CORRUPT;
    }
    for (u32 i = 0; i < NPH_MATERIAL_TEXTURE_COUNT; ++i) {
        const NphMaterialTextureRef *t = &h->textures[i];
        if (t->wrap_u > (u8)NPH_MATERIAL_WRAP_MIRROR || t->wrap_v > (u8)NPH_MATERIAL_WRAP_MIRROR ||
            t->nearest > 1U) {
            return NPH_RESULT_CORRUPT;
        }
    }
    out->header = h;
    return NPH_RESULT_OK;
}

void nph_material_header_default(NphMaterialHeader *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    out->base_color_factor = nph_vec4_one();
    out->emissive_factor = nph_vec3_zero();
    out->emissive_strength = 1.0F;
    out->metallic_factor = 1.0F;
    out->roughness_factor = 1.0F;
    out->alpha_cutoff = 0.5F;
    out->alpha_mode = (u32)NPH_MATERIAL_ALPHA_OPAQUE;
    for (u32 i = 0; i < NPH_MATERIAL_TEXTURE_COUNT; ++i) {
        out->textures[i].scale = 1.0F;
    }
}

// ---------------------------------------------------------------------------
// Scene
// ---------------------------------------------------------------------------

NphResult nph_scene_view(const NphAsset *asset, NphSceneView *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    NPH_TRY(check_type(asset, NPH_ASSET_TYPE_SCENE, NPH_SCENE_TYPE_VERSION));
    const void *p = NULL;
    NPH_TRY(typed_blob(asset, NPH_SCENE_TAG_HEADER, 0U, sizeof(NphSceneHeader), 1U, &p));
    const NphSceneHeader *h = (const NphSceneHeader *)p;
    NPH_TRY(typed_blob(asset, NPH_SCENE_TAG_NODES, 0U, sizeof(NphSceneNode), h->node_count, &p));
    const NphSceneNode *nodes = (const NphSceneNode *)p;
    NPH_TRY(typed_blob(asset, NPH_SCENE_TAG_INSTANCES, 0U, sizeof(NphSceneMeshInstance),
                       h->instance_count, &p));
    const NphSceneMeshInstance *instances = (const NphSceneMeshInstance *)p;
    NPH_TRY(typed_blob(asset, NPH_SCENE_TAG_MATERIALS, 0U, sizeof(NphGuid), h->material_count, &p));
    const NphGuid *materials = (const NphGuid *)p;
    NPH_TRY(
        typed_blob(asset, NPH_SCENE_TAG_CAMERAS, 0U, sizeof(NphSceneCamera), h->camera_count, &p));
    const NphSceneCamera *cameras = (const NphSceneCamera *)p;
    NPH_TRY(typed_blob(asset, NPH_SCENE_TAG_LIGHTS, 0U, sizeof(NphSceneLight), h->light_count, &p));
    const NphSceneLight *lights = (const NphSceneLight *)p;
    NPH_TRY(typed_blob(asset, NPH_SCENE_TAG_STRINGS, 0U, 1U, h->string_bytes, &p));
    const char *strings = (const char *)p;
    if (h->string_bytes != 0U && strings[h->string_bytes - 1U] != '\0') {
        return NPH_RESULT_CORRUPT;
    }
    const u32 n = h->node_count;
    for (u32 i = 0; i < n; ++i) {
        const NphSceneNode *node = &nodes[i];
        if ((node->parent != NPH_SCENE_NO_NODE && node->parent >= i) ||
            (node->first_child != NPH_SCENE_NO_NODE && node->first_child >= n) ||
            (node->next_sibling != NPH_SCENE_NO_NODE && node->next_sibling >= n) ||
            node->name >= h->string_bytes) {
            return NPH_RESULT_CORRUPT;
        }
    }
    for (u32 i = 0; i < h->instance_count; ++i) {
        const NphSceneMeshInstance *inst = &instances[i];
        if (inst->node >= n || inst->material_offset > h->material_count ||
            inst->material_count > h->material_count - inst->material_offset) {
            return NPH_RESULT_CORRUPT;
        }
    }
    for (u32 i = 0; i < h->camera_count; ++i) {
        if (cameras[i].node >= n || cameras[i].type > (u32)NPH_SCENE_CAMERA_ORTHOGRAPHIC) {
            return NPH_RESULT_CORRUPT;
        }
    }
    for (u32 i = 0; i < h->light_count; ++i) {
        if (lights[i].node >= n || lights[i].type > (u32)NPH_SCENE_LIGHT_SPOT) {
            return NPH_RESULT_CORRUPT;
        }
    }
    out->header = h;
    out->nodes = nodes;
    out->instances = instances;
    out->materials = materials;
    out->cameras = cameras;
    out->lights = lights;
    out->strings = strings;
    return NPH_RESULT_OK;
}

NphStr nph_scene_node_name(const NphSceneView *view, u32 index)
{
    NPH_ASSERT(view != NULL && index < view->header->node_count);
    return nph_str_from_cstr(view->strings + view->nodes[index].name);
}

// ---------------------------------------------------------------------------
// Sound
// ---------------------------------------------------------------------------

u32 nph_sound_format_bytes(u32 format)
{
    switch (format) {
    case NPH_SOUND_FORMAT_S16:
        return 2U;
    case NPH_SOUND_FORMAT_S24:
        return 3U;
    case NPH_SOUND_FORMAT_S32:
    case NPH_SOUND_FORMAT_F32:
        return 4U;
    default:
        return 0U;
    }
}

// The codec's own fields: PCM's format, depth, and size; ADPCM's depth and
// memory-only rule; lossless's depth and blocks, which tile DATA in order
// (offsets rising from 0, every block at least a byte), so a stream that
// reads block b reads [offsets[b], offsets[b + 1]) inside it.
static NphResult sound_codec_check(const NphAsset *asset, const NphSoundHeader *h, u64 size,
                                   const u32 **out_offsets)
{
    *out_offsets = NULL;
    if (h->codec != NPH_SOUND_CODEC_LOSSLESS && (h->block_frames != 0U || h->block_count != 0U)) {
        return NPH_RESULT_CORRUPT;
    }
    switch (h->codec) {
    case NPH_SOUND_CODEC_PCM: {
        // frames <= 2^40 and channels * bytes <= 32: the product fits.
        const u32 bytes = nph_sound_format_bytes(h->format);
        if (bytes == 0U || h->bits != bytes * 8U || size != h->frames * h->channels * bytes) {
            return NPH_RESULT_CORRUPT;
        }
        return NPH_RESULT_OK;
    }
    case NPH_SOUND_CODEC_ADPCM:
        // Its exact size is the codec's to check, when a clip is made.
        if (h->format != NPH_SOUND_FORMAT_NONE || h->bits != 16U ||
            (h->flags & NPH_SOUND_FLAG_STREAM) != 0U) {
            return NPH_RESULT_CORRUPT;
        }
        return NPH_RESULT_OK;
    case NPH_SOUND_CODEC_LOSSLESS: {
        if (h->format != NPH_SOUND_FORMAT_NONE || h->bits < 4U || h->bits > 24U ||
            h->block_frames == 0U ||
            (u64)h->block_count != (h->frames + h->block_frames - 1U) / h->block_frames) {
            return NPH_RESULT_CORRUPT;
        }
        const void *p = NULL;
        NPH_TRY(typed_blob(asset, NPH_SOUND_TAG_BLOCKS, 0U, sizeof(u32), h->block_count, &p));
        const u32 *offsets = (const u32 *)p;
        if (offsets == NULL || offsets[0] != 0U) {
            return NPH_RESULT_CORRUPT;
        }
        for (u32 b = 0U; b < h->block_count; ++b) {
            const u64 next = b + 1U < h->block_count ? offsets[b + 1U] : size;
            if (offsets[b] >= next || next > size) {
                return NPH_RESULT_CORRUPT;
            }
        }
        *out_offsets = offsets;
        return NPH_RESULT_OK;
    }
    default:
        return NPH_RESULT_CORRUPT;
    }
}

NphResult nph_sound_view(const NphAsset *asset, NphSoundView *out)
{
    NPH_ASSERT(out != NULL);
    memset(out, 0, sizeof *out);
    NPH_TRY(check_type(asset, NPH_ASSET_TYPE_SOUND, NPH_SOUND_TYPE_VERSION));
    const void *p = NULL;
    NPH_TRY(typed_blob(asset, NPH_SOUND_TAG_HEADER, 0U, sizeof(NphSoundHeader), 1U, &p));
    const NphSoundHeader *h = (const NphSoundHeader *)p;
    const bool stream = (h->flags & NPH_SOUND_FLAG_STREAM) != 0U;
    if (h->channels == 0U || h->channels > NPH_SOUND_MAX_CHANNELS ||
        h->sample_rate < NPH_SOUND_MIN_RATE || h->sample_rate > NPH_SOUND_MAX_RATE ||
        h->frames == 0U ||
        h->frames > (stream ? NPH_SOUND_MAX_STREAM_FRAMES : NPH_SOUND_MAX_FRAMES) ||
        h->loop_start >= h->loop_end || h->loop_end > h->frames ||
        (h->flags & ~(u32)NPH_SOUND_FLAG_STREAM) != 0U || h->reserved[0] != 0U ||
        h->reserved[1] != 0U) {
        return NPH_RESULT_CORRUPT;
    }
    // Stored: a stream reads the samples from the file, where a compressed
    // blob's bytes are not them.
    const NphAssetBlob *data = nph_asset_blob(asset, NPH_SOUND_TAG_DATA, 0U);
    if (data == NULL || data->size == 0U || data->codec != NPH_ASSET_CODEC_NONE) {
        return NPH_RESULT_CORRUPT;
    }
    const u32 *offsets = NULL;
    NPH_TRY(sound_codec_check(asset, h, data->size, &offsets));
    out->header = h;
    out->data = (const u8 *)data->data;
    out->size = data->size;
    out->offset = data->offset;
    out->block_offsets = offsets;
    return NPH_RESULT_OK;
}

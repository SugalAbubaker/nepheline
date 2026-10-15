// SPDX-License-Identifier: Apache-2.0
// asset_viewer: the Phase 6 sample (CLAUDE.md §12, "a glTF scene cooks and
// loads"). samples/asset_viewer/content/showcase.gltf is source content;
// the build cooks it with nph_cook into a pack (samples/CMakeLists.txt),
// and this program uses the pack the way a game does:
//
//   - a VFS mounts the pack and the asset loader brings in the scene by its
//     GUID on the job system, then the meshes and materials the scene
//     names, then the textures the materials name, each batch in flight
//     together;
//   - each mesh's cooked streams are uploaded as they are (16-bit
//     positions, octahedral normals, half-float texture coordinates) and
//     each texture's whole mip chain in its block-compressed format;
//   - every mesh instance is drawn at its node's world transform with its
//     material, the vertex shader decoding the streams
//     (shaders/samples/cooked_mesh.vert), lit by the scene's directional
//     light while the camera orbits.
//
//     asset_viewer [--frames=N] [--pack=FILE] [--capture=FILE.bmp] [--vsync]
//
// --capture writes the last frame of a --frames run as a 24-bit BMP, read
// back from the swapchain image (the CTest run keeps one in the build
// tree, a picture of what the cooked content looks like on that device).
//
// Exit status 77: no window system or Vulkan device (CTest "skipped");
// 1: the content did not load or a validation error was reported.

#include <nepheline/asset/material.h>
#include <nepheline/asset/mesh.h>
#include <nepheline/asset/scene.h>
#include <nepheline/asset/texture.h>
#include <nepheline/asset/vfs.h>
#include <nepheline/gpu/render_graph.h>
#include <nepheline/gpu/upload.h>
#include <nepheline/jobs/jobs.h>
#include <nepheline/math/mat.h>
#include <nepheline/math/scalar.h>
#include <nepheline/platform/file.h>
#include <nepheline/platform/log_thread.h>
#include <nepheline/platform/platform.h>
#include <nepheline/platform/time.h>
#include <nepheline/platform/window.h>

#include <nph_shared.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

NPH_LOG_CATEGORY_DEFINE(LOG_SAMPLE, "asset_viewer", NPH_LOG_LEVEL_INFO);

#define EXIT_SKIPPED 77
#define MAX_ASSETS   64U
#define MAX_MESHES   16U
#define MAX_TEXTURES 16U
#define MAX_DRAWS    64U

// The scene's identity: the source's path under the cook root and the
// sub-asset, as the cooker names it (docs/cook.md, "Identity").
#define SCENE_GUID_KEY "showcase.gltf#scene"

NPH_STATIC_ASSERT(sizeof(NphCookedDraw) == 144U, "NphCookedDraw matches its GLSL layout");
NPH_STATIC_ASSERT(sizeof(NphCookedPush) <= NPH_GPU_PUSH_CONSTANT_BYTES, "push constants fit");

typedef struct GpuMesh {
    NphGuid guid;
    NphMeshView view; // valid while the loader holds the asset
    NphGpuBuffer positions;
    NphGpuBuffer attribs;
    NphGpuBuffer indices;
} GpuMesh;

typedef struct GpuTexture {
    NphGuid guid;
    NphGpuImage image;
} GpuTexture;

typedef struct Draw {
    u32 mesh; // into Viewer.meshes
    u32 first_index;
    u32 index_count;
} Draw;

typedef struct Viewer {
    NphJobSystem *jobs;
    NphAssetLoader *loader;
    NphGuid requested[MAX_ASSETS];
    NphAssetHandle handles[MAX_ASSETS];
    u32 asset_count;
    NphSceneView scene;
    NphVec3 light_dir; // towards the light

    NphGpuDevice *dev;
    GpuMesh meshes[MAX_MESHES];
    u32 mesh_count;
    GpuTexture textures[MAX_TEXTURES];
    u32 texture_count;
    NphGpuImage white;
    NphGpuSampler sampler;
    NphGpuBuffer draw_buffer;
    Draw draws[MAX_DRAWS];
    u32 draw_count;
    NphGpuShader vert;
    NphGpuShader frag;
    NphGpuPipeline pipeline;
    NphCookedPush push;
    NphGpuBuffer capture;      // READBACK: the captured frame's texels
    NphGpuImage capture_image; // the backbuffer it is copied from
} Viewer;

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

// Starts loading `guid` unless it was requested before (a material shared
// by two instances is loaded once). `done` is signalled when it finishes.
static NphResult request(Viewer *v, NphGuid guid, NphJobCounter *done)
{
    for (u32 i = 0; i < v->asset_count; ++i) {
        if (nph_guid_equals(v->requested[i], guid)) {
            return NPH_RESULT_OK;
        }
    }
    if (v->asset_count == MAX_ASSETS) {
        return NPH_RESULT_FULL;
    }
    NphAssetHandle h = NPH_ASSET_HANDLE_NULL;
    NPH_TRY(nph_asset_load_guid_async(v->loader, guid, done, &h));
    v->requested[v->asset_count] = guid;
    v->handles[v->asset_count] = h;
    ++v->asset_count;
    return NPH_RESULT_OK;
}

// Waits for every load in flight and reports the first that failed.
static NphResult wait_loads(Viewer *v, NphJobCounter *done)
{
    nph_job_wait(v->jobs, done);
    for (u32 i = 0; i < v->asset_count; ++i) {
        const NphResult r = nph_asset_wait(v->loader, v->handles[i]);
        if (r != NPH_RESULT_OK) {
            char guid[NPH_GUID_STRING_LEN + 1U];
            nph_guid_to_string(v->requested[i], guid);
            NPH_LOG_ERROR(&LOG_SAMPLE, "asset %s: %s", guid, nph_result_string(r));
            return r;
        }
    }
    return NPH_RESULT_OK;
}

static const NphAsset *asset_by_guid(Viewer *v, NphGuid guid)
{
    return nph_asset_get(v->loader, nph_asset_find_guid(v->loader, guid));
}

// The scene, then what it references, then what those reference: the
// order a game streams a level in.
static NphResult load_content(Viewer *v)
{
    NphJobCounter done = NPH_JOB_COUNTER_INIT;
    NPH_TRY(request(v, nph_guid_from_cstr(SCENE_GUID_KEY), &done));
    NPH_TRY(wait_loads(v, &done));
    NPH_TRY(nph_scene_view(nph_asset_get(v->loader, v->handles[0]), &v->scene));
    const NphSceneHeader *sh = v->scene.header;
    for (u32 i = 0; i < sh->instance_count; ++i) {
        NPH_TRY(request(v, v->scene.instances[i].mesh, &done));
    }
    for (u32 i = 0; i < sh->material_count; ++i) {
        NPH_TRY(request(v, v->scene.materials[i], &done));
    }
    NPH_TRY(wait_loads(v, &done));
    for (u32 i = 0; i < sh->material_count; ++i) {
        NphMaterialView mv;
        NPH_TRY(nph_material_view(asset_by_guid(v, v->scene.materials[i]), &mv));
        for (u32 t = 0; t < NPH_MATERIAL_TEXTURE_COUNT; ++t) {
            if (!nph_guid_is_null(mv.header->textures[t].texture)) {
                NPH_TRY(request(v, mv.header->textures[t].texture, &done));
            }
        }
    }
    return wait_loads(v, &done);
}

// ---------------------------------------------------------------------------
// The scene's transforms and light
// ---------------------------------------------------------------------------

// World transforms of every node: parents come before their children in a
// cooked scene, so one pass suffices.
static void node_world_matrices(const NphSceneView *sv, NphMat4 *out)
{
    for (u32 i = 0; i < sv->header->node_count; ++i) {
        const NphSceneNode *n = &sv->nodes[i];
        const NphMat4 local = nph_mat4_from_trs(n->translation, n->rotation, n->scale);
        out[i] = n->parent == NPH_SCENE_NO_NODE ? local : nph_mat4_mul(out[n->parent], local);
    }
}

// The direction towards the scene's first directional light (glTF lights
// shine along their node's -Z), or a default from above.
static NphVec3 light_direction(const NphSceneView *sv, const NphMat4 *world)
{
    for (u32 i = 0; i < sv->header->light_count; ++i) {
        const NphSceneLight *l = &sv->lights[i];
        if (l->type == NPH_SCENE_LIGHT_DIRECTIONAL) {
            const NphVec3 shine = nph_mat4_transform_vector(world[l->node], nph_vec3(0, 0, -1));
            return nph_vec3_normalize_or(nph_vec3_neg(shine), nph_vec3_unit_y());
        }
    }
    return nph_vec3_normalize(nph_vec3(0.4F, 0.8F, 0.5F));
}

// ---------------------------------------------------------------------------
// GPU resources
// ---------------------------------------------------------------------------

static NphGpuFormat gpu_format(u32 format)
{
    switch (format) {
    case NPH_TEXTURE_FORMAT_R8_UNORM:
        return NPH_GPU_FORMAT_R8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8_UNORM:
        return NPH_GPU_FORMAT_R8G8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8B8A8_UNORM:
        return NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    case NPH_TEXTURE_FORMAT_R8G8B8A8_SRGB:
        return NPH_GPU_FORMAT_R8G8B8A8_SRGB;
    case NPH_TEXTURE_FORMAT_R16G16B16A16_SFLOAT:
        return NPH_GPU_FORMAT_R16G16B16A16_SFLOAT;
    case NPH_TEXTURE_FORMAT_BC4_UNORM:
        return NPH_GPU_FORMAT_BC4_UNORM;
    case NPH_TEXTURE_FORMAT_BC5_UNORM:
        return NPH_GPU_FORMAT_BC5_UNORM;
    case NPH_TEXTURE_FORMAT_BC6H_UFLOAT:
        return NPH_GPU_FORMAT_BC6H_UFLOAT;
    case NPH_TEXTURE_FORMAT_BC7_UNORM:
        return NPH_GPU_FORMAT_BC7_UNORM;
    case NPH_TEXTURE_FORMAT_BC7_SRGB:
        return NPH_GPU_FORMAT_BC7_SRGB;
    default:
        return NPH_GPU_FORMAT_UNDEFINED;
    }
}

static NphResult make_buffer(NphGpuDevice *dev, const void *data, u64 size, u32 usage,
                             const char *name, NphGpuBuffer *out)
{
    NphGpuBufferDesc bd;
    memset(&bd, 0, sizeof bd);
    bd.size = size;
    bd.usage = usage | (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
    bd.name = name;
    NPH_TRY(nph_gpu_buffer_create(dev, &bd, out));
    return nph_gpu_upload_buffer(dev, *out, 0U, data, size);
}

static NphResult upload_mesh(Viewer *v, NphGuid guid)
{
    if (v->mesh_count == MAX_MESHES) {
        return NPH_RESULT_FULL;
    }
    GpuMesh *m = &v->meshes[v->mesh_count];
    m->guid = guid;
    NPH_TRY(nph_mesh_view(asset_by_guid(v, guid), &m->view));
    const NphMeshHeader *h = m->view.header;
    ++v->mesh_count; // from here on its buffers are released with the others
    NPH_TRY(make_buffer(v->dev, m->view.positions, (u64)h->vertex_count * sizeof(NphMeshPosition),
                        (u32)NPH_GPU_BUFFER_USAGE_STORAGE, "cooked positions", &m->positions));
    NPH_TRY(make_buffer(v->dev, m->view.attribs, (u64)h->vertex_count * sizeof(NphMeshAttrib),
                        (u32)NPH_GPU_BUFFER_USAGE_STORAGE, "cooked attributes", &m->attribs));
    return make_buffer(v->dev, m->view.indices, (u64)h->index_count * sizeof(u32),
                       (u32)NPH_GPU_BUFFER_USAGE_INDEX, "cooked indices", &m->indices);
}

static NphResult upload_texture(Viewer *v, NphGuid guid)
{
    if (v->texture_count == MAX_TEXTURES) {
        return NPH_RESULT_FULL;
    }
    NphTextureView tv;
    NPH_TRY(nph_texture_view(asset_by_guid(v, guid), &tv));
    const NphTextureHeader *h = tv.header;
    const NphGpuFormat format = gpu_format(h->format);
    if (format == NPH_GPU_FORMAT_UNDEFINED ||
        (nph_texture_format_info(h->format)->compressed &&
         !nph_gpu_device_info(v->dev)->features.texture_compression_bc)) {
        NPH_LOG_WARN(&LOG_SAMPLE, "%s textures are not supported here; drawing untextured",
                     nph_texture_format_info(h->format)->name);
        return NPH_RESULT_OK;
    }
    GpuTexture *t = &v->textures[v->texture_count];
    t->guid = guid;
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.format = format;
    id.width = h->width;
    id.height = h->height;
    id.mip_count = h->mip_count;
    id.layer_count = h->layer_count;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    id.name = "cooked texture";
    NPH_TRY(nph_gpu_image_create(v->dev, &id, &t->image));
    ++v->texture_count;
    for (u32 mip = 0; mip < h->mip_count; ++mip) {
        NPH_TRY(nph_gpu_upload_image(v->dev, t->image, mip, 0U, h->layer_count, tv.mips[mip],
                                     tv.mip_sizes[mip]));
    }
    return NPH_RESULT_OK;
}

static u32 texture_index(const Viewer *v, NphGuid guid)
{
    for (u32 i = 0; i < v->texture_count; ++i) {
        if (nph_guid_equals(v->textures[i].guid, guid)) {
            return nph_gpu_image_sampled_index(v->dev, v->textures[i].image);
        }
    }
    return nph_gpu_image_sampled_index(v->dev, v->white);
}

static u32 mesh_index(const Viewer *v, NphGuid guid)
{
    for (u32 i = 0; i < v->mesh_count; ++i) {
        if (nph_guid_equals(v->meshes[i].guid, guid)) {
            return i;
        }
    }
    return NPH_U32_MAX;
}

// One draw record per submesh of every instance, with its node's world
// transform and its material's base colour.
static NphResult build_draws(Viewer *v, const NphMat4 *world, NphCookedDraw *records)
{
    const NphSceneView *sv = &v->scene;
    for (u32 i = 0; i < sv->header->instance_count; ++i) {
        const NphSceneMeshInstance *inst = &sv->instances[i];
        const u32 mi = mesh_index(v, inst->mesh);
        if (mi == NPH_U32_MAX) {
            return NPH_RESULT_NOT_FOUND;
        }
        const GpuMesh *m = &v->meshes[mi];
        const NphMeshHeader *h = m->view.header;
        for (u32 s = 0; s < h->submesh_count; ++s) {
            if (v->draw_count == MAX_DRAWS) {
                return NPH_RESULT_FULL;
            }
            const NphSubmesh *sub = &m->view.submeshes[s];
            NphMaterialHeader material;
            nph_material_header_default(&material);
            if (sub->material_slot < inst->material_count) {
                NphMaterialView mv;
                NPH_TRY(nph_material_view(
                    asset_by_guid(v, sv->materials[inst->material_offset + sub->material_slot]),
                    &mv));
                material = *mv.header;
            }
            NphCookedDraw *r = &records[v->draw_count];
            memset(r, 0, sizeof *r);
            r->model = world[inst->node];
            r->base_color = material.base_color_factor;
            r->position_offset = nph_vec4_from_vec3(h->position_offset, 0.0F);
            r->position_scale = nph_vec4_from_vec3(h->position_scale, 0.0F);
            r->positions_address = nph_gpu_buffer_address(v->dev, m->positions);
            r->attribs_address = nph_gpu_buffer_address(v->dev, m->attribs);
            r->texture_index =
                texture_index(v, material.textures[NPH_MATERIAL_TEXTURE_BASE_COLOR].texture);
            v->draws[v->draw_count].mesh = mi;
            v->draws[v->draw_count].first_index = sub->index_offset;
            v->draws[v->draw_count].index_count = sub->index_count;
            ++v->draw_count;
        }
    }
    return NPH_RESULT_OK;
}

static NphResult setup_gpu(Viewer *v, NphGpuFormat color_format)
{
    NphAllocator *alloc = nph_allocator_system();
    // Meshes and textures, straight from the loaded assets.
    for (u32 i = 0; i < v->asset_count; ++i) {
        const NphAsset *a = nph_asset_get(v->loader, v->handles[i]);
        if (a->header.type == NPH_ASSET_TYPE_MESH) {
            NPH_TRY(upload_mesh(v, v->requested[i]));
        } else if (a->header.type == NPH_ASSET_TYPE_TEXTURE) {
            NPH_TRY(upload_texture(v, v->requested[i]));
        }
    }
    // A white texel for untextured materials.
    NphGpuImageDesc id;
    memset(&id, 0, sizeof id);
    id.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    id.width = 1U;
    id.height = 1U;
    id.mip_count = 1U;
    id.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    id.name = "white";
    NPH_TRY(nph_gpu_image_create(v->dev, &id, &v->white));
    static const u8 white[4] = {255U, 255U, 255U, 255U};
    NPH_TRY(nph_gpu_upload_image(v->dev, v->white, 0U, 0U, 1U, white, sizeof white));

    // The draw records.
    const u32 nodes = v->scene.header->node_count;
    NphMat4 *world = NPH_ALLOC_ARRAY(alloc, NphMat4, nodes != 0U ? nodes : 1U);
    NphCookedDraw *records = NPH_ALLOC_ARRAY(alloc, NphCookedDraw, MAX_DRAWS);
    if (world == NULL || records == NULL) {
        NPH_FREE_ARRAY(alloc, NphMat4, world, nodes != 0U ? nodes : 1U);
        NPH_FREE_ARRAY(alloc, NphCookedDraw, records, MAX_DRAWS);
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    node_world_matrices(&v->scene, world);
    v->light_dir = light_direction(&v->scene, world);
    NphResult r = build_draws(v, world, records);
    if (r == NPH_RESULT_OK && v->draw_count != 0U) {
        r = make_buffer(v->dev, records, (u64)v->draw_count * sizeof(NphCookedDraw),
                        (u32)NPH_GPU_BUFFER_USAGE_STORAGE, "draw records", &v->draw_buffer);
    }
    NPH_FREE_ARRAY(alloc, NphMat4, world, nodes != 0U ? nodes : 1U);
    NPH_FREE_ARRAY(alloc, NphCookedDraw, records, MAX_DRAWS);
    NPH_TRY(r);
    NPH_TRY(nph_gpu_upload_flush(v->dev, NULL));

    NphGpuSamplerDesc sd;
    nph_gpu_sampler_desc_default(&sd);
    sd.max_anisotropy = 8.0F;
    sd.name = "trilinear aniso";
    NPH_TRY(nph_gpu_sampler_create(v->dev, &sd, &v->sampler));
    char path[512];
    snprintf(path, sizeof path, "%s/cooked_mesh.vert.spv", NPH_SHADER_DIR);
    NPH_TRY(nph_gpu_shader_load(v->dev, path, "cooked_mesh.vert", &v->vert));
    snprintf(path, sizeof path, "%s/cooked_mesh.frag.spv", NPH_SHADER_DIR);
    NPH_TRY(nph_gpu_shader_load(v->dev, path, "cooked_mesh.frag", &v->frag));
    NphGpuGraphicsPipelineDesc gp;
    memset(&gp, 0, sizeof gp);
    gp.vertex = v->vert;
    gp.fragment = v->frag;
    gp.cull_mode = NPH_GPU_CULL_BACK;
    gp.color_formats[0] = color_format;
    gp.color_count = 1U;
    gp.depth_format = NPH_GPU_FORMAT_D32_SFLOAT;
    gp.depth.test = 1;
    gp.depth.write = 1;
    gp.depth.compare = NPH_GPU_COMPARE_GREATER; // reverse-Z
    gp.name = "cooked mesh";
    NPH_TRY(nph_gpu_pipeline_create_graphics(v->dev, &gp, &v->pipeline));
    v->push.draws_address = nph_gpu_buffer_address(v->dev, v->draw_buffer);
    v->push.sampler_index = nph_gpu_sampler_index(v->dev, v->sampler);
    v->push.light_dir = nph_vec4_from_vec3(v->light_dir, 0.0F);
    return NPH_RESULT_OK;
}

static void destroy_gpu(Viewer *v)
{
    NphGpuDevice *dev = v->dev;
    nph_gpu_buffer_destroy(dev, v->capture);
    nph_gpu_pipeline_destroy(dev, v->pipeline);
    nph_gpu_shader_destroy(dev, v->vert);
    nph_gpu_shader_destroy(dev, v->frag);
    nph_gpu_sampler_destroy(dev, v->sampler);
    nph_gpu_buffer_destroy(dev, v->draw_buffer);
    nph_gpu_image_destroy(dev, v->white);
    for (u32 i = 0; i < v->texture_count; ++i) {
        nph_gpu_image_destroy(dev, v->textures[i].image);
    }
    for (u32 i = 0; i < v->mesh_count; ++i) {
        nph_gpu_buffer_destroy(dev, v->meshes[i].positions);
        nph_gpu_buffer_destroy(dev, v->meshes[i].attribs);
        nph_gpu_buffer_destroy(dev, v->meshes[i].indices);
    }
}

static void draw_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    Viewer *v = (Viewer *)user;
    nph_gpu_cmd_bind_pipeline(cmd, v->pipeline);
    for (u32 i = 0; i < v->draw_count; ++i) {
        const Draw *d = &v->draws[i];
        v->push.draw_index = i;
        nph_gpu_cmd_push_constants(cmd, &v->push, sizeof v->push);
        nph_gpu_cmd_bind_index_buffer(cmd, v->meshes[d->mesh].indices, 0U, NPH_GPU_INDEX_U32);
        nph_gpu_cmd_draw_indexed(cmd, d->index_count, 1U, d->first_index, 0, 0U);
    }
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

static void capture_execute(NphGpuRenderGraph *g, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(g);
    Viewer *v = (Viewer *)user;
    NphGpuBufferImageCopy region;
    memset(&region, 0, sizeof region); // all of mip 0, tightly packed
    nph_gpu_cmd_copy_image_to_buffer(cmd, v->capture_image, NPH_GPU_LAYOUT_TRANSFER_SRC, v->capture,
                                     &region, 1U);
}

static void put_le(u8 *p, u32 v, u32 bytes)
{
    for (u32 i = 0; i < bytes; ++i) {
        p[i] = (u8)(v >> (8U * i));
    }
}

// A 24-bit BMP of 8-bit BGRA (or RGBA) texels: rows bottom-up, each padded
// to four bytes.
static NphResult write_bmp(const char *path, const u8 *texels, u32 width, u32 height, bool bgra)
{
    NphAllocator *alloc = nph_allocator_system();
    const u64 row = ((u64)width * 3U + 3U) & ~(u64)3U;
    const u64 size = 54U + row * height;
    if (size > NPH_U32_MAX) {
        return NPH_RESULT_OVERFLOW;
    }
    u8 *file = NPH_ALLOC_ARRAY_ZEROED(alloc, u8, size);
    if (file == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    file[0] = 'B';
    file[1] = 'M';
    put_le(file + 2, (u32)size, 4U);
    put_le(file + 10, 54U, 4U); // the pixels' offset
    put_le(file + 14, 40U, 4U); // BITMAPINFOHEADER
    put_le(file + 18, width, 4U);
    put_le(file + 22, height, 4U); // positive: bottom-up
    put_le(file + 26, 1U, 2U);     // planes
    put_le(file + 28, 24U, 2U);    // bits per pixel
    put_le(file + 34, (u32)(row * height), 4U);
    for (u32 y = 0; y < height; ++y) {
        const u8 *src = texels + (u64)(height - 1U - y) * width * 4U;
        u8 *dst = file + 54U + row * y;
        for (u32 x = 0; x < width; ++x) {
            const u8 *t = src + (u64)x * 4U;
            dst[(u64)x * 3U] = bgra ? t[0] : t[2];
            dst[(u64)x * 3U + 1U] = t[1];
            dst[(u64)x * 3U + 2U] = bgra ? t[2] : t[0];
        }
    }
    const NphResult r = nph_file_write_all(path, file, size);
    NPH_FREE_ARRAY(alloc, u8, file, size);
    return r;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char **argv)
{
    u64 max_frames = 0U;
    const char *pack_path = NPH_SAMPLE_PACK;
    const char *capture_path = NULL;
    bool vsync = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--frames=", 9U) == 0) {
            max_frames = strtoull(argv[i] + 9, NULL, 10);
        } else if (strncmp(argv[i], "--pack=", 7U) == 0) {
            pack_path = argv[i] + 7;
        } else if (strncmp(argv[i], "--capture=", 10U) == 0) {
            capture_path = argv[i] + 10;
        } else if (strcmp(argv[i], "--vsync") == 0) {
            vsync = true;
        } else {
            fprintf(stderr, "usage: asset_viewer [--frames=N] [--pack=FILE] [--capture=FILE.bmp] "
                            "[--vsync]\n");
            return 2;
        }
    }
    if (capture_path != NULL && max_frames == 0U) {
        fprintf(stderr, "asset_viewer: --capture needs --frames (it captures the last one)\n");
        return 2;
    }
    NphAllocator *alloc = nph_allocator_system();
    NphLogConfig log_config;
    memset(&log_config, 0, sizeof log_config);
    log_config.alloc = alloc;
    if (nph_log_init(&log_config) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogConsoleSink console;
    console.color = nph_platform_console_enable_vt();
    console.with_location = false;
    if (nph_log_add_sink(nph_log_console_sink, &console, NPH_LOG_LEVEL_INFO) != NPH_RESULT_OK) {
        return 1;
    }
    NphLogThread log_thread;
    memset(&log_thread, 0, sizeof log_thread);
    if (nph_log_thread_start(&log_thread, 5U) != NPH_RESULT_OK) {
        return 1;
    }

    int status = 0;
    Viewer viewer;
    memset(&viewer, 0, sizeof viewer);
    Viewer *v = &viewer;
    NphVfs *vfs = NULL;
    NphWindow *window = NULL;
    NphGpuSwapchain *swapchain = NULL;
    NphGpuRenderGraph *graph = NULL;
    bool gpu_ready = false;

    // The content: mount the pack and load the scene and everything it uses.
    NphJobSystemDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.alloc = alloc;
    NphAssetLoaderDesc ld;
    memset(&ld, 0, sizeof ld);
    ld.alloc = alloc;
    ld.verify = NPH_ASSET_VERIFY_ALWAYS; // checksum every blob: this is a demonstration
    NphResult r = nph_job_system_create(&jd, &v->jobs);
    if (r == NPH_RESULT_OK) {
        r = nph_vfs_create(alloc, &vfs);
    }
    if (r == NPH_RESULT_OK) {
        r = nph_vfs_mount_pack(vfs, NPH_STR("content"), pack_path, NULL);
        if (r != NPH_RESULT_OK) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "cannot mount %s: %s (the build cooks it)", pack_path,
                          nph_result_string(r));
        }
    }
    if (r == NPH_RESULT_OK) {
        ld.vfs = vfs;
        ld.jobs = v->jobs;
        r = nph_asset_loader_create(&ld, &v->loader);
    }
    const NphStopwatch load_time = nph_stopwatch_start();
    if (r == NPH_RESULT_OK) {
        r = load_content(v);
    }
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "loading the content failed: %s", nph_result_string(r));
        status = 1;
        goto done;
    }
    NPH_LOG_INFO(&LOG_SAMPLE,
                 "%s: %u assets loaded in %.1f ms (%u nodes, %u mesh instances, %u materials, "
                 "%u lights)",
                 pack_path, v->asset_count, nph_stopwatch_elapsed_ms(&load_time),
                 v->scene.header->node_count, v->scene.header->instance_count,
                 v->scene.header->material_count, v->scene.header->light_count);

    // The window and the device.
    if (nph_window_system_init() != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no window system available; skipping");
        status = EXIT_SKIPPED;
        goto done;
    }
    NphWindowDesc wd;
    nph_window_desc_default(&wd);
    wd.title = "Nepheline asset_viewer";
    if (nph_window_create(alloc, &wd, &window) != NPH_RESULT_OK) {
        status = EXIT_SKIPPED;
        goto done;
    }
    NphGpuDeviceDesc dd;
    memset(&dd, 0, sizeof dd);
    dd.alloc = alloc;
    dd.app_name = "asset_viewer";
    r = nph_gpu_device_create(&dd, &v->dev);
    if (r != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "no Vulkan device (%s); skipping", nph_result_string(r));
        status = r == NPH_RESULT_UNSUPPORTED ? EXIT_SKIPPED : 1;
        goto done;
    }
    NphGpuSwapchainDesc scd;
    memset(&scd, 0, sizeof scd);
    scd.window = window;
    scd.vsync = vsync;
    scd.prefer_srgb = 1;
    if (nph_gpu_swapchain_create(v->dev, &scd, &swapchain) != NPH_RESULT_OK) {
        NPH_LOG_WARN(&LOG_SAMPLE, "the window cannot be presented to; skipping");
        status = EXIT_SKIPPED;
        goto done;
    }
    gpu_ready = true; // partial resources are released below
    r = setup_gpu(v, nph_gpu_swapchain_format(swapchain));
    if (r != NPH_RESULT_OK) {
        NPH_LOG_ERROR(&LOG_SAMPLE, "GPU setup failed: %s", nph_result_string(r));
        status = 1;
        goto done;
    }
    NphGpuRenderGraphDesc gd;
    memset(&gd, 0, sizeof gd);
    gd.alloc = alloc;
    gd.name = "asset viewer";
    if (nph_gpu_render_graph_create(v->dev, &gd, &graph) != NPH_RESULT_OK) {
        status = 1;
        goto done;
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%s: %u draws of %u meshes and %u textures; Escape quits",
                 nph_gpu_device_info(v->dev)->name, v->draw_count, v->mesh_count, v->texture_count);

    u64 frame = 0U;
    const NphStopwatch run = nph_stopwatch_start();
    while (!nph_window_should_close(window) && (max_frames == 0U || frame < max_frames)) {
        if (nph_gpu_frame_begin(v->dev) != NPH_RESULT_OK) {
            status = 1;
            break;
        }
        nph_window_poll_events();
        NphWindowEvent e;
        while (nph_window_pop_event(window, &e)) {
            if (e.type == NPH_WINDOW_EVENT_KEY && e.u.key.key == NPH_KEY_ESCAPE &&
                e.u.key.action == NPH_KEY_ACTION_PRESS) {
                nph_window_set_should_close(window, true);
            }
        }
        NphGpuImage backbuffer = NPH_GPU_IMAGE_NULL;
        const NphResult ar = nph_gpu_swapchain_acquire(swapchain, &backbuffer);
        if (ar == NPH_RESULT_EMPTY) {
            NPH_IGNORE_RESULT(nph_gpu_frame_end(v->dev));
            nph_thread_sleep_ns(16U * NPH_TIME_NS_PER_MS);
            continue;
        }
        if (ar != NPH_RESULT_OK) {
            status = 1;
            NPH_IGNORE_RESULT(nph_gpu_frame_end(v->dev));
            break;
        }
        u32 width = 0U;
        u32 height = 0U;
        nph_gpu_swapchain_extent(swapchain, &width, &height);

        // An orbiting camera (right-handed, Y up, reverse-Z: ADR-0008).
        const f32 t = (f32)(nph_stopwatch_elapsed_ms(&run) / 1000.0) * 0.4F;
        const NphVec3 eye = nph_vec3(6.0F * nph_f32_cos(t), 3.2F, 6.0F * nph_f32_sin(t));
        const NphMat4 view =
            nph_mat4_look_at(eye, nph_vec3(0.0F, 0.8F, 0.0F), nph_vec3(0.0F, 1.0F, 0.0F));
        const NphMat4 proj =
            nph_mat4_perspective_infinite(55.0F * NPH_DEG_TO_RAD, (f32)width / (f32)height, 0.1F);
        v->push.view_proj = nph_mat4_mul(proj, view);

        nph_gpu_rg_begin(graph);
        NphGpuRgImage target = nph_gpu_rg_import_image(graph, backbuffer);
        NphGpuImageDesc depth_desc;
        memset(&depth_desc, 0, sizeof depth_desc);
        depth_desc.format = NPH_GPU_FORMAT_D32_SFLOAT;
        depth_desc.width = width;
        depth_desc.height = height;
        depth_desc.mip_count = 1U;
        depth_desc.usage = (u32)NPH_GPU_IMAGE_USAGE_DEPTH_ATTACHMENT;
        depth_desc.name = "depth";
        NphGpuRgImage depth = nph_gpu_rg_create_image(graph, &depth_desc);
        NphGpuRgPassDesc pd;
        memset(&pd, 0, sizeof pd);
        pd.name = "scene";
        pd.type = NPH_GPU_RG_PASS_GRAPHICS;
        pd.execute = draw_execute;
        pd.user = v;
        NphGpuRgPass *pass = nph_gpu_rg_add_pass(graph, &pd);
        NphGpuRgAttachment color;
        memset(&color, 0, sizeof color);
        color.load = NPH_GPU_LOAD_OP_CLEAR;
        color.store = NPH_GPU_STORE_OP_STORE;
        color.clear = nph_gpu_clear_color(0.05F, 0.06F, 0.09F, 1.0F);
        nph_gpu_rg_color_attachment(pass, target, &color);
        NphGpuRgAttachment ds;
        memset(&ds, 0, sizeof ds);
        ds.load = NPH_GPU_LOAD_OP_CLEAR;
        ds.store = NPH_GPU_STORE_OP_DONT_CARE;
        ds.clear = nph_gpu_clear_depth(0.0F, 0U); // reverse-Z: far is 0
        nph_gpu_rg_depth_attachment(pass, depth, &ds, 0);
        const bool capturing = capture_path != NULL && frame + 1U == max_frames &&
                               (nph_gpu_image_desc(v->dev, backbuffer)->usage &
                                (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_SRC) != 0U;
        if (capturing) {
            NphGpuBufferDesc cbd;
            memset(&cbd, 0, sizeof cbd);
            cbd.size = (u64)width * height * 4U;
            cbd.usage = (u32)NPH_GPU_BUFFER_USAGE_TRANSFER_DST;
            cbd.memory = NPH_GPU_MEMORY_READBACK;
            cbd.name = "capture";
            if (nph_gpu_buffer_create(v->dev, &cbd, &v->capture) == NPH_RESULT_OK) {
                v->capture_image = backbuffer;
                memset(&pd, 0, sizeof pd);
                pd.name = "capture";
                pd.type = NPH_GPU_RG_PASS_TRANSFER;
                pd.execute = capture_execute;
                pd.user = v;
                NphGpuRgPass *copy = nph_gpu_rg_add_pass(graph, &pd);
                nph_gpu_rg_read_image(copy, target, NPH_GPU_RG_USE_TRANSFER_SRC, 0U, 1U);
                nph_gpu_rg_write_buffer(copy, nph_gpu_rg_import_buffer(graph, v->capture),
                                        NPH_GPU_RG_USE_TRANSFER_DST);
            }
        }
        nph_gpu_rg_present(graph, target, swapchain);
        if (nph_gpu_rg_compile(graph) != NPH_RESULT_OK ||
            nph_gpu_rg_execute(graph) != NPH_RESULT_OK ||
            nph_gpu_swapchain_present(swapchain) != NPH_RESULT_OK ||
            nph_gpu_frame_end(v->dev) != NPH_RESULT_OK) {
            status = 1;
            break;
        }
        ++frame;
    }
    NPH_LOG_INFO(&LOG_SAMPLE, "%llu frames in %.1f ms", (unsigned long long)frame,
                 nph_stopwatch_elapsed_ms(&run));
    if (capture_path != NULL && status == 0) {
        if (nph_gpu_buffer_is_valid(v->dev, v->capture) &&
            nph_gpu_device_wait_idle(v->dev) == NPH_RESULT_OK) {
            u32 width = 0U;
            u32 height = 0U;
            nph_gpu_swapchain_extent(swapchain, &width, &height);
            const NphGpuFormat format = nph_gpu_swapchain_format(swapchain);
            const bool bgra =
                format == NPH_GPU_FORMAT_B8G8R8A8_SRGB || format == NPH_GPU_FORMAT_B8G8R8A8_UNORM;
            r = write_bmp(capture_path, (const u8 *)nph_gpu_buffer_map(v->dev, v->capture), width,
                          height, bgra);
            if (r == NPH_RESULT_OK) {
                NPH_LOG_INFO(&LOG_SAMPLE, "captured the last frame to %s", capture_path);
            } else {
                NPH_LOG_ERROR(&LOG_SAMPLE, "%s: %s", capture_path, nph_result_string(r));
                status = 1;
            }
        } else {
            NPH_LOG_WARN(&LOG_SAMPLE, "the swapchain images cannot be read back; no capture");
        }
    }

done:
    if (v->dev != NULL) {
        NPH_IGNORE_RESULT(nph_gpu_device_wait_idle(v->dev));
        nph_gpu_render_graph_destroy(graph);
        if (gpu_ready) {
            destroy_gpu(v);
        }
        nph_gpu_swapchain_destroy(swapchain);
        const NphGpuValidationStats vs = nph_gpu_device_validation_stats(v->dev);
        if (vs.errors != 0U) {
            NPH_LOG_ERROR(&LOG_SAMPLE, "%llu validation errors", (unsigned long long)vs.errors);
            status = 1;
        }
        nph_gpu_device_destroy(v->dev);
    }
    nph_window_destroy(window);
    nph_window_system_shutdown();
    for (u32 i = 0; i < v->asset_count; ++i) {
        nph_asset_release(v->loader, v->handles[i]);
    }
    nph_asset_loader_destroy(v->loader);
    nph_vfs_destroy(vfs);
    nph_job_system_destroy(v->jobs);
    nph_log_thread_stop(&log_thread);
    nph_log_shutdown();
    return status;
}

// SPDX-License-Identifier: Apache-2.0
// Dear ImGui's renderer backend on the engine's RHI. See ui_renderer.h.
#include "ui_renderer.h"

#include <nepheline/core/assert.h>
#include <nepheline/core/log.h>
#include <nepheline/gpu/commands.h>
#include <nepheline/gpu/device.h>
#include <nepheline/gpu/pipeline.h>
#include <nepheline/gpu/resources.h>
#include <nepheline/gpu/shader.h>
#include <nepheline/gpu/upload.h>

#include "editor_log.h"

#include <stdio.h>
#include <string.h>

#define UI_MIN_BUFFER_BYTES ((u64)64U * 1024U)

// Shaders: shaders/editor/ui_common.glsl.
typedef struct UiPush {
    f32 scale[2];
    f32 translate[2];
    u64 vertices;
    u32 texture_index;
    u32 sampler_index;
    u32 flags;
    u32 pad;
} UiPush;

#define UI_FLAG_OPAQUE 1U

typedef struct UiFrameBuffers {
    NphGpuBuffer vertices;
    u64 vertex_bytes;
    NphGpuBuffer indices;
    u64 index_bytes;
} UiFrameBuffers;

// What a texture ImGui manages carries for the backend.
typedef struct UiTexture {
    NphGpuImage image;
} UiTexture;

struct UiRenderer {
    NphGpuDevice *dev;
    NphAllocator *alloc;
    NphGpuShader vert;
    NphGpuShader frag;
    NphGpuPipeline pipeline;
    NphGpuSampler sampler;
    UiFrameBuffers frames[NPH_GPU_FRAMES_IN_FLIGHT];
    ImDrawData *draw_data; // prepared last
    UiFrameBuffers *prepared;
    bool warned_callback;
};

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

ImTextureID ui_renderer_texture_id(const UiRenderer *r, NphGpuImage image, bool opaque)
{
    NPH_ASSERT(r != NULL);
    const u64 index = (u64)nph_gpu_image_sampled_index(r->dev, image);
    return (ImTextureID)((index + 1U) | (opaque ? UI_TEXTURE_OPAQUE : 0U));
}

static void texture_destroy(UiRenderer *r, ImTextureData *tex)
{
    UiTexture *t = (UiTexture *)tex->BackendUserData;
    if (t != NULL) {
        nph_gpu_image_destroy(r->dev, t->image); // deferred past the frames that use it
        NPH_FREE_TYPE(r->alloc, UiTexture, t);
        tex->BackendUserData = NULL;
    }
    ImTextureData_SetTexID(tex, 0U);
    ImTextureData_SetStatus(tex, ImTextureStatus_Destroyed);
}

// Copies the rectangle of `tex`'s pixels into `out` as RGBA8, tightly
// packed (ImGui's Alpha8 textures become white with that alpha).
static void gather_rgba(ImTextureData *tex, u32 x, u32 y, u32 w, u32 h, u8 *out)
{
    for (u32 row = 0; row < h; ++row) {
        const u8 *src = (const u8 *)ImTextureData_GetPixelsAt(tex, (int)x, (int)(y + row));
        u8 *dst = out + (u64)row * w * 4U;
        if (tex->Format == ImTextureFormat_RGBA32) {
            memcpy(dst, src, (size_t)w * 4U);
        } else {
            for (u32 i = 0; i < w; ++i) {
                dst[i * 4U + 0U] = 255U;
                dst[i * 4U + 1U] = 255U;
                dst[i * 4U + 2U] = 255U;
                dst[i * 4U + 3U] = src[i];
            }
        }
    }
}

static NphResult upload_rect(UiRenderer *r, ImTextureData *tex, const UiTexture *t, u32 x, u32 y,
                             u32 w, u32 h)
{
    const u64 bytes = (u64)w * h * 4U;
    u8 *pixels = (u8 *)NPH_ALLOC(r->alloc, bytes, 16U);
    if (pixels == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    gather_rgba(tex, x, y, w, h, pixels);
    const NphResult res =
        nph_gpu_upload_image_region(r->dev, t->image, 0U, 0U, x, y, w, h, pixels, bytes);
    NPH_FREE(r->alloc, pixels, bytes, 16U);
    return res;
}

static NphResult texture_create(UiRenderer *r, ImTextureData *tex)
{
    UiTexture *t = NPH_ALLOC_TYPE(r->alloc, UiTexture);
    if (t == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    NphGpuImageDesc d;
    memset(&d, 0, sizeof d);
    d.format = NPH_GPU_FORMAT_R8G8B8A8_UNORM;
    d.width = (u32)tex->Width;
    d.height = (u32)tex->Height;
    d.mip_count = 1U;
    d.usage = (u32)NPH_GPU_IMAGE_USAGE_SAMPLED | (u32)NPH_GPU_IMAGE_USAGE_TRANSFER_DST;
    d.name = "editor ui texture";
    NphResult res = nph_gpu_image_create(r->dev, &d, &t->image);
    if (res == NPH_RESULT_OK) {
        tex->BackendUserData = t;
        res = upload_rect(r, tex, t, 0U, 0U, d.width, d.height);
    } else {
        NPH_FREE_TYPE(r->alloc, UiTexture, t);
    }
    if (res != NPH_RESULT_OK) {
        texture_destroy(r, tex);
        return res;
    }
    ImTextureData_SetTexID(tex, ui_renderer_texture_id(r, t->image, false));
    ImTextureData_SetStatus(tex, ImTextureStatus_OK);
    return NPH_RESULT_OK;
}

static NphResult texture_update(UiRenderer *r, ImTextureData *tex)
{
    const UiTexture *t = (const UiTexture *)tex->BackendUserData;
    if (t == NULL) {
        return texture_create(r, tex);
    }
    for (int i = 0; i < tex->Updates.Size; ++i) {
        const ImTextureRect *u = &tex->Updates.Data[i];
        NPH_TRY(upload_rect(r, tex, t, u->x, u->y, u->w, u->h));
    }
    ImTextureData_SetStatus(tex, ImTextureStatus_OK);
    return NPH_RESULT_OK;
}

static NphResult apply_textures(UiRenderer *r, ImDrawData *dd)
{
    if (dd->Textures == NULL) {
        return NPH_RESULT_OK;
    }
    for (int i = 0; i < dd->Textures->Size; ++i) {
        ImTextureData *tex = dd->Textures->Data[i];
        switch (tex->Status) {
        case ImTextureStatus_WantCreate:
            NPH_TRY(texture_create(r, tex));
            break;
        case ImTextureStatus_WantUpdates:
            NPH_TRY(texture_update(r, tex));
            break;
        case ImTextureStatus_WantDestroy:
            texture_destroy(r, tex);
            break;
        case ImTextureStatus_OK:
        case ImTextureStatus_Destroyed:
        default:
            break;
        }
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

static u64 grow(u64 have, u64 need)
{
    u64 cap = have > 0U ? have : UI_MIN_BUFFER_BYTES;
    while (cap < need) {
        cap *= 2U;
    }
    return cap;
}

static NphResult ensure_buffer(UiRenderer *r, NphGpuBuffer *buffer, u64 *bytes, u64 need, u32 usage,
                               const char *name)
{
    if (need <= *bytes && nph_gpu_buffer_is_valid(r->dev, *buffer)) {
        return NPH_RESULT_OK;
    }
    nph_gpu_buffer_destroy(r->dev, *buffer); // deferred past the frames that read it
    *buffer = NPH_GPU_BUFFER_NULL;
    NphGpuBufferDesc d;
    memset(&d, 0, sizeof d);
    d.size = grow(*bytes, need);
    d.usage = usage;
    d.memory = NPH_GPU_MEMORY_UPLOAD;
    d.name = name;
    NPH_TRY(nph_gpu_buffer_create(r->dev, &d, buffer));
    *bytes = d.size;
    return NPH_RESULT_OK;
}

NphResult ui_renderer_prepare(UiRenderer *r, ImDrawData *dd)
{
    NPH_ASSERT(r != NULL && dd != NULL);
    r->draw_data = NULL;
    r->prepared = NULL;
    NPH_TRY(apply_textures(r, dd));
    UiFrameBuffers *fb = &r->frames[nph_gpu_frame_slot(r->dev)];
    const u64 vbytes = (u64)dd->TotalVtxCount * sizeof(ImDrawVert);
    const u64 ibytes = (u64)dd->TotalIdxCount * sizeof(ImDrawIdx);
    if (vbytes > 0U) {
        NPH_TRY(ensure_buffer(r, &fb->vertices, &fb->vertex_bytes, vbytes,
                              (u32)NPH_GPU_BUFFER_USAGE_STORAGE, "editor ui vertices"));
        NPH_TRY(ensure_buffer(r, &fb->indices, &fb->index_bytes, ibytes,
                              (u32)NPH_GPU_BUFFER_USAGE_INDEX, "editor ui indices"));
        u8 *v = (u8 *)nph_gpu_buffer_map(r->dev, fb->vertices);
        u8 *ix = (u8 *)nph_gpu_buffer_map(r->dev, fb->indices);
        NPH_ASSERT(v != NULL && ix != NULL);
        for (int l = 0; l < dd->CmdLists.Size; ++l) {
            const ImDrawList *list = dd->CmdLists.Data[l];
            const u64 lv = (u64)list->VtxBuffer.Size * sizeof(ImDrawVert);
            const u64 li = (u64)list->IdxBuffer.Size * sizeof(ImDrawIdx);
            memcpy(v, list->VtxBuffer.Data, (size_t)lv);
            memcpy(ix, list->IdxBuffer.Data, (size_t)li);
            v += lv;
            ix += li;
        }
    }
    r->draw_data = dd;
    r->prepared = fb;
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void execute(NphGpuRenderGraph *graph, NphGpuCmd *cmd, void *user)
{
    NPH_UNUSED(graph);
    UiRenderer *r = (UiRenderer *)user;
    ImDrawData *dd = r->draw_data;
    if (dd == NULL || dd->TotalVtxCount == 0) {
        return; // the pass still clears the target
    }
    const f32 fb_w = dd->DisplaySize.x * dd->FramebufferScale.x;
    const f32 fb_h = dd->DisplaySize.y * dd->FramebufferScale.y;
    if (fb_w <= 0.0F || fb_h <= 0.0F) {
        return;
    }
    nph_gpu_cmd_set_viewport(cmd, 0.0F, 0.0F, fb_w, fb_h, 0.0F, 1.0F);
    nph_gpu_cmd_bind_pipeline(cmd, r->pipeline);
    nph_gpu_cmd_bind_index_buffer(cmd, r->prepared->indices, 0U, NPH_GPU_INDEX_U16);
    UiPush push;
    memset(&push, 0, sizeof push);
    push.scale[0] = 2.0F / dd->DisplaySize.x;
    push.scale[1] = 2.0F / dd->DisplaySize.y;
    push.translate[0] = -1.0F - dd->DisplayPos.x * push.scale[0];
    push.translate[1] = -1.0F - dd->DisplayPos.y * push.scale[1];
    push.vertices = nph_gpu_buffer_address(r->dev, r->prepared->vertices);
    push.sampler_index = nph_gpu_sampler_index(r->dev, r->sampler);
    const ImVec2_c origin = dd->DisplayPos;
    const ImVec2_c scale = dd->FramebufferScale;
    u32 global_vtx = 0U;
    u32 global_idx = 0U;
    for (int l = 0; l < dd->CmdLists.Size; ++l) {
        ImDrawList *list = dd->CmdLists.Data[l];
        for (int c = 0; c < list->CmdBuffer.Size; ++c) {
            ImDrawCmd *pcmd = &list->CmdBuffer.Data[c];
            if (pcmd->UserCallback != NULL) {
                if (!r->warned_callback) {
                    NPH_LOG_WARN(&LOG_EDITOR, "ImGui draw callbacks are not supported; skipped");
                    r->warned_callback = true;
                }
                continue;
            }
            // The clip rectangle in framebuffer pixels, clamped to it.
            f32 x0 = (pcmd->ClipRect.x - origin.x) * scale.x;
            f32 y0 = (pcmd->ClipRect.y - origin.y) * scale.y;
            f32 x1 = (pcmd->ClipRect.z - origin.x) * scale.x;
            f32 y1 = (pcmd->ClipRect.w - origin.y) * scale.y;
            x0 = x0 < 0.0F ? 0.0F : x0;
            y0 = y0 < 0.0F ? 0.0F : y0;
            x1 = x1 > fb_w ? fb_w : x1;
            y1 = y1 > fb_h ? fb_h : y1;
            if (x1 <= x0 || y1 <= y0 || pcmd->ElemCount == 0U) {
                continue;
            }
            nph_gpu_cmd_set_scissor(cmd, (i32)x0, (i32)y0, (u32)(x1 - x0), (u32)(y1 - y0));
            const u64 id = (u64)ImDrawCmd_GetTexID(pcmd);
            push.texture_index = (u32)((id & 0xFFFFFFFFU) - 1U);
            push.flags = (id & UI_TEXTURE_OPAQUE) != 0U ? UI_FLAG_OPAQUE : 0U;
            nph_gpu_cmd_push_constants(cmd, &push, (u32)sizeof push);
            nph_gpu_cmd_draw_indexed(cmd, pcmd->ElemCount, 1U, pcmd->IdxOffset + global_idx,
                                     (i32)(pcmd->VtxOffset + global_vtx), 0U);
        }
        global_vtx += (u32)list->VtxBuffer.Size;
        global_idx += (u32)list->IdxBuffer.Size;
    }
}

NphResult ui_renderer_draw(UiRenderer *r, NphGpuRenderGraph *graph, NphGpuRgImage target,
                           const NphGpuRgImage *reads, u32 read_count)
{
    NPH_ASSERT(r != NULL && graph != NULL);
    NphGpuRgPassDesc pd;
    memset(&pd, 0, sizeof pd);
    pd.name = "editor ui";
    pd.type = NPH_GPU_RG_PASS_GRAPHICS;
    pd.execute = execute;
    pd.user = r;
    NphGpuRgPass *pass = nph_gpu_rg_add_pass(graph, &pd);
    if (pass == NULL) {
        return NPH_RESULT_FULL;
    }
    NphGpuRgAttachment a;
    memset(&a, 0, sizeof a);
    a.load = NPH_GPU_LOAD_OP_CLEAR;
    a.store = NPH_GPU_STORE_OP_STORE;
    a.clear = nph_gpu_clear_color(0.08F, 0.08F, 0.09F, 1.0F);
    nph_gpu_rg_color_attachment(pass, target, &a);
    for (u32 i = 0; i < read_count; ++i) {
        nph_gpu_rg_read_image(pass, reads[i], NPH_GPU_RG_USE_SAMPLED, 0U, 1U);
    }
    return NPH_RESULT_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

NphResult ui_renderer_create(NphGpuDevice *dev, NphAllocator *alloc, const char *shader_dir,
                             NphGpuFormat target_format, UiRenderer **out)
{
    NPH_ASSERT(dev != NULL && alloc != NULL && shader_dir != NULL && out != NULL);
    *out = NULL;
    UiRenderer *r = NPH_ALLOC_TYPE(alloc, UiRenderer);
    if (r == NULL) {
        return NPH_RESULT_OUT_OF_MEMORY;
    }
    r->dev = dev;
    r->alloc = alloc;
    char path[600];
    (void)snprintf(path, sizeof path, "%s/ui.vert.spv", shader_dir);
    NphResult res = nph_gpu_shader_load(dev, path, "ui.vert", &r->vert);
    if (res == NPH_RESULT_OK) {
        (void)snprintf(path, sizeof path, "%s/ui.frag.spv", shader_dir);
        res = nph_gpu_shader_load(dev, path, "ui.frag", &r->frag);
    }
    if (res == NPH_RESULT_OK) {
        NphGpuGraphicsPipelineDesc d;
        memset(&d, 0, sizeof d);
        d.vertex = r->vert;
        d.fragment = r->frag;
        d.topology = NPH_GPU_TOPOLOGY_TRIANGLE_LIST;
        d.cull_mode = NPH_GPU_CULL_NONE;
        d.color_formats[0] = target_format;
        d.color_count = 1U;
        d.blend[0].enable = 1;
        d.blend[0].src_color = NPH_GPU_BLEND_SRC_ALPHA;
        d.blend[0].dst_color = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
        d.blend[0].color_op = NPH_GPU_BLEND_OP_ADD;
        d.blend[0].src_alpha = NPH_GPU_BLEND_ONE;
        d.blend[0].dst_alpha = NPH_GPU_BLEND_ONE_MINUS_SRC_ALPHA;
        d.blend[0].alpha_op = NPH_GPU_BLEND_OP_ADD;
        d.name = "editor ui";
        res = nph_gpu_pipeline_create_graphics(dev, &d, &r->pipeline);
    }
    if (res == NPH_RESULT_OK) {
        NphGpuSamplerDesc sd;
        nph_gpu_sampler_desc_default(&sd);
        sd.min_filter = NPH_GPU_FILTER_LINEAR;
        sd.mag_filter = NPH_GPU_FILTER_LINEAR;
        sd.address_u = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
        sd.address_v = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
        sd.address_w = NPH_GPU_ADDRESS_CLAMP_TO_EDGE;
        sd.name = "editor ui";
        res = nph_gpu_sampler_create(dev, &sd, &r->sampler);
    }
    if (res != NPH_RESULT_OK) {
        ui_renderer_destroy(r);
        return res;
    }
    *out = r;
    return NPH_RESULT_OK;
}

void ui_renderer_destroy(UiRenderer *r)
{
    if (r == NULL) {
        return;
    }
    // The textures ImGui still holds (its font atlas, at least).
    if (igGetCurrentContext() != NULL) {
        ImGuiPlatformIO *pio = igGetPlatformIO_Nil();
        for (int i = 0; i < pio->Textures.Size; ++i) {
            ImTextureData *tex = pio->Textures.Data[i];
            if (tex->BackendUserData != NULL) {
                texture_destroy(r, tex);
            }
        }
    }
    for (u32 i = 0; i < NPH_GPU_FRAMES_IN_FLIGHT; ++i) {
        nph_gpu_buffer_destroy(r->dev, r->frames[i].vertices);
        nph_gpu_buffer_destroy(r->dev, r->frames[i].indices);
    }
    nph_gpu_sampler_destroy(r->dev, r->sampler);
    nph_gpu_pipeline_destroy(r->dev, r->pipeline);
    nph_gpu_shader_destroy(r->dev, r->vert);
    nph_gpu_shader_destroy(r->dev, r->frag);
    NPH_FREE_TYPE(r->alloc, UiRenderer, r);
}

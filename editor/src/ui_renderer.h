// SPDX-License-Identifier: Apache-2.0
#pragma once

// Dear ImGui's renderer backend on the engine's RHI (docs/editor.md "UI";
// docs/adr/0025-editor-ui.md). It owns:
//   - the textures ImGui asks for (ImGuiBackendFlags_RendererHasTextures,
//     Dear ImGui 1.92): created, updated by rectangles, and destroyed as the
//     draw data's texture list requests;
//   - per frame slot, a host-visible vertex and index buffer the frame's
//     draw lists are copied into;
//   - one pipeline for the swapchain's format.
// A frame's draw data is drawn by one render-graph pass into the backbuffer
// (cleared first). Draw commands name textures by ImTextureID: the
// bindless sampled index plus one (zero stays "no texture"), with
// UI_TEXTURE_OPAQUE set for images whose alpha means nothing (the scene
// viewport).
//
// Thread-safety: NPH_MAIN_THREAD_ONLY.

#include <nepheline/gpu/render_graph.h>

#include "imgui_c.h"

#define UI_TEXTURE_OPAQUE (1ULL << 32U)

typedef struct UiRenderer UiRenderer;

// NPH_RESULT_NOT_FOUND or NPH_RESULT_IO_ERROR when the shaders are missing;
// the device's errors. `shader_dir` holds ui.vert.spv and ui.frag.spv.
NPH_MUST_USE NphResult ui_renderer_create(NphGpuDevice *dev, NphAllocator *alloc,
                                          const char *shader_dir, NphGpuFormat target_format,
                                          UiRenderer **out);

// Destroys every texture ImGui still holds and the renderer's resources.
// The device must be idle. Safe on NULL.
void ui_renderer_destroy(UiRenderer *r);

// The ImTextureID that shows `image` (sampled), opaque or not.
ImTextureID ui_renderer_texture_id(const UiRenderer *r, NphGpuImage image, bool opaque);

// Applies the draw data's texture requests and copies its geometry into
// this frame's buffers. Inside a frame, before the graph is compiled.
NPH_MUST_USE NphResult ui_renderer_prepare(UiRenderer *r, ImDrawData *draw_data);

// Adds the pass that clears `target` and draws the draw data prepared last.
// `reads` lists images the draw data samples that the graph tracks (the
// scene viewport): the pass reads them as sampled images.
NPH_MUST_USE NphResult ui_renderer_draw(UiRenderer *r, NphGpuRenderGraph *graph,
                                        NphGpuRgImage target, const NphGpuRgImage *reads,
                                        u32 read_count);

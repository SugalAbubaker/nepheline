// SPDX-License-Identifier: Apache-2.0
// The editor UI's draw data (editor/src/ui_renderer.c): Dear ImGui's
// vertices, read through their buffer's address, and a bindless texture
// per draw command.
#ifndef UI_COMMON_GLSL
#define UI_COMMON_GLSL

#include "nph_bindless.glsl"

// ImDrawVert: position, texture coordinate, RGBA8 colour (20 bytes).
struct UiVertex {
    vec2 pos;
    vec2 uv;
    uint color;
};

layout(buffer_reference, scalar, buffer_reference_align = 4) readonly buffer UiVertices
{
    UiVertex v[];
};

// UI_FLAG_OPAQUE: the texture's alpha is ignored (a scene viewport, whose
// tonemapped colour carries no meaningful alpha).
#define UI_FLAG_OPAQUE 1u

layout(push_constant, scalar) uniform UiPush
{
    vec2 scale;     // 2 / display size
    vec2 translate; // -1 - display position * scale
    uint64_t vertices;
    uint texture_index;
    uint sampler_index;
    uint flags;
    uint pad;
} pc;

#endif

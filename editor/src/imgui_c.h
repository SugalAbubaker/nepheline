// SPDX-License-Identifier: Apache-2.0
#pragma once

// The editor's one door to Dear ImGui: cimgui's C declarations (the
// nph_cimgui target defines CIMGUI_DEFINE_ENUMS_AND_STRUCTS for its users),
// and a few helpers for the by-value vector types C code builds a lot.

#include <nepheline/core/types.h>

#include <cimgui.h>

static inline ImVec2_c ui_vec2(f32 x, f32 y)
{
    ImVec2_c v;
    v.x = x;
    v.y = y;
    return v;
}

static inline ImVec4_c ui_vec4(f32 x, f32 y, f32 z, f32 w)
{
    ImVec4_c v;
    v.x = x;
    v.y = y;
    v.z = z;
    v.w = w;
    return v;
}

// A texture reference to a texture the editor manages (the scene
// viewport), as opposed to one ImGui manages (its font atlas).
static inline ImTextureRef_c ui_texture_ref(ImTextureID id)
{
    ImTextureRef_c r;
    r._TexData = NULL;
    r._TexID = id;
    return r;
}

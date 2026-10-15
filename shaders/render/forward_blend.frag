// SPDX-License-Identifier: Apache-2.0
// Forward shading of blended materials (glTF alphaMode BLEND): writes the
// material's alpha for "over" blending after the opaque geometry
// (forward_frag.glsl).
#version 460
#define NPH_FORWARD_ALPHA_MODE 2
#include "forward_frag.glsl"

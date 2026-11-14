// SPDX-License-Identifier: Apache-2.0
// forward_blend.frag for the production path's blended surfaces, whose draws
// are the GPU's (forward_frag.glsl NPH_FORWARD_INDIRECT; ADR-0097). It reads
// gl_PrimitiveID, which the production path's devices have (the reference
// renderer's shaders ask for nothing of the kind).
#version 460
#extension GL_EXT_demote_to_helper_invocation : require
#define NPH_FORWARD_ALPHA_MODE 2
#define NPH_FORWARD_INDIRECT
#include "forward_frag.glsl"

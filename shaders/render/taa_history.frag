// SPDX-License-Identifier: Apache-2.0
// The temporal resolve into the next history alone, which the motion blur
// reads and tonemaps (taa_resolve.glsl, motion_blur.frag).
#version 460
#define TAA_HISTORY_ONLY
#include "taa_resolve.glsl"

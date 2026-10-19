// SPDX-License-Identifier: Apache-2.0
// The motion blur's motion image (docs/render.md "Motion blur"; ADR-0036):
// a pixel's blur and its surface's view distance packed in one 32-bit word,
// half the bytes of half floats, which motion.comp writes for every pixel
// and every sample of the gather (motion_blur.frag) reads.
//
//   bits 0-9, 10-19   the blur's half-extent in x and y, in sixteenths of a
//                     pixel, offset by 512: up to 31.94 pixels either way (a
//                     blur reaches at most a tile, NPH_RENDER_MB_TILE)
//   bits 20-31        log2 of the view distance in 64ths of an octave,
//                     offset by 16 octaves: 1.1 % steps (the depth test's
//                     softness is 5 %) from 15 micrometres to beyond the
//                     sky's 65504 m
#ifndef NPH_MOTION_COMMON_GLSL
#define NPH_MOTION_COMMON_GLSL

#define NPH_MOTION_STEPS 16.0                       // per pixel of blur
#define NPH_MOTION_MAX   (511.0 / NPH_MOTION_STEPS) // the longest blur component kept

uint nph_motion_pack(vec2 blur, float distance)
{
    const uvec2 b =
        uvec2(clamp(round(blur * NPH_MOTION_STEPS), vec2(-511.0), vec2(511.0)) + 512.0);
    const uint d = uint(clamp(round((log2(distance) + 16.0) * 64.0), 0.0, 4095.0));
    return b.x | (b.y << 10u) | (d << 20u);
}

vec2 nph_motion_blur(uint m)
{
    return (vec2(uvec2(m, m >> 10u) & 1023u) - 512.0) / NPH_MOTION_STEPS;
}

float nph_motion_distance(uint m)
{
    return exp2(float(m >> 20u) / 64.0 - 16.0);
}

#endif

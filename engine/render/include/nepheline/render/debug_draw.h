// SPDX-License-Identifier: Apache-2.0
#pragma once

// Immediate-mode debug lines (CLAUDE.md §8.6 "a debug-draw API"; §8.9
// "Debug visualization through the render debug-draw API").
//
// Lines added before a frame's nph_render_frame are drawn by that frame
// over the tonemapped image and then discarded: call the functions again
// every frame a shape should stay visible. Each line is either depth
// tested against the scene (the default) or drawn on top of it
// (NPH_RENDER_DEBUG_OVERLAY).
//
// Positions are 32-bit, relative to the debug origin (a 64-bit world
// position, the world origin by default): a system that draws far from
// the world origin sets the origin near what it draws, so its lines keep
// full precision (CLAUDE.md §8.2).
//
// Storage: three buffers of NphRendererDesc.debug_lines lines per list,
// persistently mapped, rotated by nph_render_frame so the one being
// written is never read by a frame in flight. A line past the capacity is
// dropped (counted in NphRenderFrameStats.debug_lines as the capacity).
//
// Thread-safety: the line functions are NPH_THREADSAFE (a slot is
// reserved with one atomic add) and may be called from job workers, but
// not while nph_render_frame runs. nph_render_debug_set_origin is
// NPH_MAIN_THREAD_ONLY and applies to the lines added after it.

#include <nepheline/math/affine.h>
#include <nepheline/render/renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

// An RGBA8 colour, sRGB encoded like any authored colour.
#define NPH_RENDER_RGBA(r, g, b, a)                                                                \
    ((u32)(r) | ((u32)(g) << 8U) | ((u32)(b) << 16U) | ((u32)(a) << 24U))

#define NPH_RENDER_COLOR_RED     NPH_RENDER_RGBA(230U, 41U, 55U, 255U)
#define NPH_RENDER_COLOR_GREEN   NPH_RENDER_RGBA(0U, 200U, 70U, 255U)
#define NPH_RENDER_COLOR_BLUE    NPH_RENDER_RGBA(40U, 110U, 255U, 255U)
#define NPH_RENDER_COLOR_YELLOW  NPH_RENDER_RGBA(250U, 220U, 40U, 255U)
#define NPH_RENDER_COLOR_CYAN    NPH_RENDER_RGBA(40U, 220U, 230U, 255U)
#define NPH_RENDER_COLOR_MAGENTA NPH_RENDER_RGBA(230U, 60U, 220U, 255U)
#define NPH_RENDER_COLOR_WHITE   NPH_RENDER_RGBA(255U, 255U, 255U, 255U)
#define NPH_RENDER_COLOR_GRAY    NPH_RENDER_RGBA(128U, 128U, 128U, 255U)

typedef enum NphRenderDebugFlags {
    NPH_RENDER_DEBUG_DEPTH_TESTED = 0,   // hidden behind scene geometry
    NPH_RENDER_DEBUG_OVERLAY = 1U << 0U, // drawn on top of everything
} NphRenderDebugFlags;

// Sets the 64-bit world position that line coordinates are relative to.
void nph_render_debug_set_origin(NphRenderer *r, NphDVec3 origin);
NphDVec3 nph_render_debug_origin(const NphRenderer *r);

// One line. False when the list is full (the line is dropped).
bool nph_render_debug_line(NphRenderer *r, NphVec3 a, NphVec3 b, u32 rgba, u32 flags);

// The twelve edges of a box.
void nph_render_debug_aabb(NphRenderer *r, NphAABB box, u32 rgba, u32 flags);
// The twelve edges of `local` transformed by `transform`.
void nph_render_debug_box(NphRenderer *r, const NphAffine *transform, NphAABB local, u32 rgba,
                          u32 flags);
// Three great circles of `segments` lines each (0: 32).
void nph_render_debug_sphere(NphRenderer *r, NphVec3 center, f32 radius, u32 rgba, u32 flags,
                             u32 segments);
// The frame's axes: X red, Y green, Z blue, `size` long.
void nph_render_debug_axes(NphRenderer *r, const NphAffine *transform, f32 size, u32 flags);
// A line with a four-line head at `to`.
void nph_render_debug_arrow(NphRenderer *r, NphVec3 from, NphVec3 to, u32 rgba, u32 flags);
// Three axis-aligned lines through `p`, `size` long.
void nph_render_debug_cross(NphRenderer *r, NphVec3 p, f32 size, u32 rgba, u32 flags);
// A square grid of `cells` x `cells` cells of `cell_size` in the XZ plane
// around `center`.
void nph_render_debug_grid(NphRenderer *r, NphVec3 center, f32 cell_size, u32 cells, u32 rgba,
                           u32 flags);
// The edges of a camera's frustum from its near plane to `far_distance`
// for the given aspect ratio (width / height).
void nph_render_debug_frustum(NphRenderer *r, const NphRenderCamera *camera, f32 aspect,
                              f32 far_distance, u32 rgba, u32 flags);

// Lines added so far for the next frame, in both lists.
u32 nph_render_debug_line_count(const NphRenderer *r);

#ifdef __cplusplus
}
#endif

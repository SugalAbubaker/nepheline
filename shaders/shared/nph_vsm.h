// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's records (engine/render/src/vsm.c; docs/render.md
// "Shadows"; ADR-0033), shared by its C code and its shaders
// (shaders/render/vsm_*). The production path's directional light that
// casts shadows (the sun) gets a clipmap of NPH_VSM_LEVELS square levels
// around the camera, each twice the side of the one before, each a window
// of NPH_VSM_WINDOW x NPH_VSM_WINDOW pages of NPH_VSM_PAGE x NPH_VSM_PAGE
// depth texels: a virtual shadow map that only the pages some pixel needs
// occupy memory in, cached from frame to frame until something they show
// changes (Fernando et al., "Adaptive Shadow Maps", SIGGRAPH 2001; Lefohn,
// Sengupta & Owens, "Resolution-Matched Shadow Maps", ACM TOG 2007; Epic
// Games, "Virtual Shadow Maps", Unreal Engine 5 documentation, 2022).
//
// Light space: the axes u, v, and the light's direction of travel d, with
// its origin at an anchor the CPU keeps within a few kilometres of the
// camera (so 32-bit coordinates hold anywhere). A level's frame shifts
// that by the level's window corner (in u and v) and depth reference (in
// d), both on its own page grid, so a page's contents stay valid as the
// camera moves: pages are named by absolute page coordinates, and a level's
// page table is a toroidal window over them (an entry holds the page whose
// coordinates equal its slot modulo the window).
#ifndef NPH_VSM_SHARED_H
#define NPH_VSM_SHARED_H

#include "nph_shared.h"

#define NPH_VSM_LEVELS        16
#define NPH_VSM_PAGE          128   // texels along a page's side
#define NPH_VSM_WINDOW        128   // pages along a level's side
#define NPH_VSM_LEVEL_ENTRIES 16384 // NPH_VSM_WINDOW^2
#define NPH_VSM_LEVEL_WORDS   512   // u32 words of a level's page bitmask
#define NPH_VSM_ROW_WORDS     4     // u32 words of a row of it, and of a level's row summary
#define NPH_VSM_POOL_ROW      64    // physical pages along a row of the pool (8192 texels)
#define NPH_VSM_NO_OWNER      0xFFFFFFFFu
#define NPH_VSM_RECENT        60 // frames: a page needed this lately is evicted last

// NphVsmEntry.state.
#define NPH_VSM_ENTRY_VALID 0x80000000u // holds a physical page
#define NPH_VSM_ENTRY_DIRTY 0x40000000u // its contents are out of date: drawn again when needed
#define NPH_VSM_ENTRY_PAGE  0x0000FFFFu // the physical page

// NphVsmCounters.overflow bits: work that did not fit and was dropped (the
// renderer then makes the levels coarser until it fits: vsm.c).
#define NPH_VSM_OVERFLOW_POOL     1 // pages needed past the pool (their pixels are lit)
#define NPH_VSM_OVERFLOW_MESHLETS 2 // max_vsm_meshlets
#define NPH_VSM_OVERFLOW_DRAWS    4 // max_vsm_draws

// vsm_pages.comp modes (NphVisPush.mode).
#define NPH_VSM_PAGES_INVALIDATE 0 // the pages this frame's instance updates touch: dirty
#define NPH_VSM_PAGES_UPDATE     1 // the page tables against this frame's windows and requests
#define NPH_VSM_PAGES_COLLECT    2 // the free and evictable physical pages
#define NPH_VSM_PAGES_ALLOC      3 // a physical page for each page that needs one
#define NPH_VSM_PAGES_FIXUP      4 // pages whose draws did not all fit: drawn again next frame
#define NPH_VSM_PAGES_ARGS       5 // indirect arguments (NphVisPush.phase: which)

// NPH_VSM_PAGES_ARGS phases.
#define NPH_VSM_ARGS_ALLOC    0 // the allocation's dispatch
#define NPH_VSM_ARGS_MESHLET  1 // the meshlet cull's dispatch
#define NPH_VSM_ARGS_DRAW     2 // the page raster's draws and clears
#define NPH_VSM_ARGS_INSTANCE 3 // the instance cull's dispatch, and its levels

// vsm_cull.comp modes (NphVisPush.mode).
#define NPH_VSM_CULL_INSTANCES 0 // (instance, level) pairs against the pages to draw (z: level)
#define NPH_VSM_CULL_MESHLETS  1 // their meshlets against those pages: the draws

// One page-table entry (a level's window slot).
NPH_SHARED_STRUCT(NphVsmEntry)
{
    u32 state;  // NPH_VSM_ENTRY_*
    i32 page_x; // the absolute page it holds
    i32 page_y;
    u32 pad0;
};

// One physical page.
NPH_SHARED_STRUCT(NphVsmPhys)
{
    u32 owner;     // the entry holding it (level * NPH_VSM_LEVEL_ENTRIES + slot), or NO_OWNER
    u32 last_used; // the frame that last needed it
};

// One level, as this frame sees it. A light-space point (camera relative)
// is at (u / page_size, v / page_size, d * depth_scale) + origin.xyz in the
// level's frame: x and y in pages from the window's first page, z the depth
// the pool stores (0 toward the light, 1 the far end of the level's range).
NPH_SHARED_STRUCT(NphVsmLevel)
{
    NphVec4 origin; // xyz: the camera in the level's frame; w: unused
    i32 window_x;   // the window's first page (absolute page coordinates)
    i32 window_y;
    f32 inv_page_size; // 1 / metres along a page's side
    f32 depth_scale;   // 1 / metres of the depth range (the window's side)
    f32 page_size;
    u32 reset; // nonzero: every page of the level is dropped this frame
    u32 pad0;
    u32 pad1;
};

// The frame's counters (cleared at the start of every frame that has the
// virtual shadow map).
NPH_SHARED_STRUCT(NphVsmCounters)
{
    u32 alloc_count;     // entries needing a physical page
    u32 free_count;      // physical pages without an owner
    u32 candidate_count; // owned pages no pixel needed lately (evicted first)
    u32 render_count;    // pages drawn this frame
    u32 meshlet_count;   // (instance, meshlet, level) items for the meshlet cull
    u32 draw_count;      // (instance, meshlet, entry) draws
    u32 overflow;        // NPH_VSM_OVERFLOW_*
    u32 recent_count;    // owned pages needed lately, not this frame (evicted last)
    // Statistics, read back two frames later.
    u32 requested; // pages this frame's pixels needed
    u32 evicted;   // pages taken from their owners
    u32 pad1;
    u32 pad2;
    u32 level_render[NPH_VSM_LEVELS]; // pages drawn per level
};

// Indirect arguments derived from the counters.
NPH_SHARED_STRUCT(NphVsmArgs)
{
    u32 alloc_args[4];          // dispatch: the allocation, an invocation per request
    u32 meshlet_args[4];        // dispatch: the meshlet cull, an invocation per item
    u32 draw_args[4];           // mesh tasks: a workgroup per draw
    u32 clear_args[4];          // VkDrawIndirectCommand: a quad (6 vertices) per page drawn
    u32 instance_args[4];       // dispatch: the instance cull, z the levels with pages to draw
    u32 levels[NPH_VSM_LEVELS]; // those levels, in order
};

// The virtual shadow map's view of a frame.
NPH_SHARED_STRUCT(NphVsmView)
{
    NphVec4 axis_u; // xyz: light space's axes, in camera-relative world space
    NphVec4 axis_v;
    NphVec4 axis_d; // the light's direction of travel
    // Light space (homogeneous) from a pixel's NDC x, y and depth: the page
    // requests rebuild each pixel's point with it.
    NphMat4 light_from_clip;
    NphVec4 pool_scale; // xy: 1 / the pool's extent in texels; zw: unused
    NphVsmLevel levels[NPH_VSM_LEVELS];
    u64 entries;     // NphVsmEntry[NPH_VSM_LEVELS * NPH_VSM_LEVEL_ENTRIES]
    u64 phys;        // NphVsmPhys[pool_pages]
    u64 requests;    // u32[NPH_VSM_LEVELS * NPH_VSM_LEVEL_WORDS]: pages this frame's pixels need
    u64 dirty;       // u32[...]: pages an instance change touched this frame
    u64 render;      // u32[...]: pages drawn this frame
    u64 render_rows; // u32[NPH_VSM_LEVELS * NPH_VSM_ROW_WORDS]: rows of `render` with a page
    u64 alloc;       // u32[pool_pages]: entries needing a page
    u64 free;        // u32[pool_pages]: pages without an owner
    u64 candidate;   // u32[pool_pages]: evictable pages, the long unused first, the recent from
                     // the end
    u64 rendered;    // u32[pool_pages]: entries drawn this frame
    u64 meshlets;    // uvec4[max_meshlets]: (instance slot, meshlet, level, unused)
    u64 draws;       // uvec4[max_draws]: (instance slot, meshlet, entry, unused)
    u64 vis_draws;   // NphVisDraw[max_draws]: the indirect raster's draws
    u64 counters;    // NphVsmCounters
    u64 args;        // NphVsmArgs
    f32 lod_scale;   // a pixel's footprint per metre of distance, over level 0's texel
    f32 texel_bias;  // the receiver's offset along its normal, in texels of its level
    f32 margin;      // pages: a request also takes the pages this near (the offset, and slack)
    u32 pool_pages;
    u32 pool_index;   // bindless sampled index of the pool (D32_SFLOAT)
    u32 pool_sampler; // bindless index of the sampler the pool is gathered with
    u32 frame;        // counts frames that have the virtual shadow map (pages' ages)
    u32 max_meshlets;
    u32 max_draws;
    u32 reset; // nonzero: every page is dropped this frame
};

#endif

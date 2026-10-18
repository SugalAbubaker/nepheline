// SPDX-License-Identifier: Apache-2.0
// The virtual shadow map's page raster (docs/render.md "Shadows"), shared by
// the mesh-shader path (vsm_raster.mesh) and the indirect-draw path
// (vsm_raster.vert): a caster's vertex in a page drawn this frame, as a
// position in the physical pool and the four clip distances that confine
// its triangles to the page's 128 x 128 texels there. The pool is one depth
// attachment of NPH_VSM_POOL_ROW pages per row; depth is the level's
// stored depth, clamped (casters nearer the light than the near plane
// still shadow). No fragment stage: the depth test keeps the nearest.
#ifndef NPH_VSM_RASTER_GLSL
#define NPH_VSM_RASTER_GLSL

#include "vis_common.glsl"
#include "vsm_common.glsl"

// The pool's extent in texels.
vec2 vsm_pool_extent(VsmViewRef r)
{
    const uint rows = (r.v.pool_pages + uint(NPH_VSM_POOL_ROW) - 1u) / uint(NPH_VSM_POOL_ROW);
    return vec2(float(NPH_VSM_POOL_ROW * NPH_VSM_PAGE), float(rows) * float(NPH_VSM_PAGE));
}

// A page drawn this frame: its level, its window-relative coordinates, and
// its first texel in the pool.
struct VsmTarget {
    int level;
    vec2 rel;
    vec2 base;
};

VsmTarget vsm_target(VsmViewRef r, uint entry)
{
    VsmTarget t;
    t.level = int(entry / uint(NPH_VSM_LEVEL_ENTRIES));
    // A page drawn this frame is the one its slot holds in this frame's
    // window.
    const ivec2 window = vsm_window(r, t.level);
    t.rel = vec2(vsm_slot_page(window, entry % uint(NPH_VSM_LEVEL_ENTRIES)) - window);
    t.base = vec2(vsm_pool_base(VsmEntriesRef(r.v.entries).e[entry].state & NPH_VSM_ENTRY_PAGE));
    return t;
}

// A camera-relative position into the page: the clip-space position and
// the four clip distances (left, right, top, bottom edges of the page).
vec4 vsm_raster_position(VsmViewRef r, VsmTarget t, vec2 extent, vec3 p, out vec4 clip)
{
    const vec3 c = vsm_level_coords(r, t.level, vsm_light_space(r, p));
    const vec2 local = (c.xy - t.rel) * float(NPH_VSM_PAGE);
    clip = vec4(local.x, float(NPH_VSM_PAGE) - local.x, local.y, float(NPH_VSM_PAGE) - local.y);
    return vec4((t.base + local) / extent * 2.0 - 1.0, c.z, 1.0);
}

#endif

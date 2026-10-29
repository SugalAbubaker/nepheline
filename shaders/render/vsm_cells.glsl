// SPDX-License-Identifier: Apache-2.0
// A world partition's cells in the sun's virtual shadow map (ADR-0062,
// ADR-0063), shared by its culling (vsm_cull.comp) and its pages'
// invalidation (vsm_pages.comp): whether a cell is drawn into a map level
// as its proxy, and the level of its proxy the map level asks for. The
// includer includes vis_common.glsl and vsm_common.glsl first.
#ifndef NPH_VSM_CELLS_GLSL
#define NPH_VSM_CELLS_GLSL

// Whether cell `cell` (its index plus one, from an instance's flags) is drawn
// into a level as its proxy (ADR-0062): with the levels of detail, where the
// proxy's finest error is within the level's threshold at scale 1
// (vsm_lod_threshold), which holds or fails the same every frame, so a page
// drawn stays right; its instances are drawn into the other levels. The sun
// is infinitely far: the proxy holds what is seen from there.
bool vsm_cell_proxied(VisViewRef view, VsmViewRef r, int level, uint cell)
{
    return (view.v.flags & NPH_VIS_VIEW_LOD) != 0u &&
           VisCellsRef(view.v.cells).c[cell - 1u].center_low.w <= vsm_lod_threshold(r, level, 1.0);
}

// The finest level of cell `cell`'s proxy the map level `level` takes, as
// it asks for it: the first whose largest consumer error is past the level's
// threshold at scale 1, brought NPH_VIS_CELL_PREFETCH nearer as the camera's
// requests are (vis_cell_request).
uint vsm_cell_request(VisViewRef view, VsmViewRef r, int level, uint cell)
{
    VisCellsRef cells = VisCellsRef(view.v.cells);
    const float T = vsm_lod_threshold(r, level, 1.0) * (1.0 - NPH_VIS_CELL_PREFETCH);
    for (uint k = 0u; k < 16u; ++k) {
        if (cells.c[cell - 1u].level_errors[k >> 2u][k & 3u] > T) {
            return k;
        }
    }
    return NPH_VIS_CELL_NO_REQUEST;
}

#endif

# 0033. The sun's virtual shadow map: a resolution-matched clipmap of cached pages

- **Date:** 2026-10-18
- **Status:** Accepted (contact shadows, decision 9, and the local lights'
  shadows go to Phase 14: [ADR-0041](0041-frame-target-gates.md))

## Context

CLAUDE.md §8.6 stage 6 asks for virtual shadow maps with page caching, so
that only pages invalidated by movement are redrawn, plus screen-space
contact shadows for fine detail; §10.4 gives shadows 0.40 ms and 80 MB of
DRAM traffic per 4K frame. [ADR-0032](0032-ray-traced-shadows.md) gave
both renderers ray-traced shadows as the ground truth, and measured them
in the production path: the sun's shadows cost 0.41 to 0.44 ms in a still
scene, all of it in the resolve's rays, and 0.79 ms once anything moves,
because the top level is then rebuilt every frame. A game always has
something moving.

The sun is the light whose shadows cover the whole view, at every
distance, so it is where a cached shadow map pays: its view of a still
scene does not change from frame to frame, and the pixels that read it
need a resolution that depends only on their distance. Local lights reach
a small part of the view each and are stage 6's next part.

Resolution-matched shadow maps choose, per pixel, the shadow-map resolution
whose texel matches the pixel's footprint, and allocate only the pages
some pixel needs (Fernando, Fernandez, Bala & Greenberg, "Adaptive Shadow
Maps", SIGGRAPH 2001; Lefohn, Sengupta & Owens, "Resolution-Matched Shadow
Maps", ACM TOG 2007). Unreal Engine 5's virtual shadow maps put that in a
clipmap of page tables over a physical page pool, caching pages from frame
to frame (Epic Games, "Virtual Shadow Maps", Unreal Engine documentation,
2022). The device features this needs, depth clamping and clip distances,
are core Vulkan features the RTX 3060 Ti and lavapipe both have, so the map
also gives the sun shadows on devices without ray queries.

Measurements are `render_perf` on the RTX 3060 Ti, Profile build, at
3840 × 2160 in the 4K field (16,385 instances, 5.3 million triangles
drawn), unless stated otherwise.

## Decision

1. **Scope.** The production path's sun, the live directional light that
   casts shadows in the lowest light slot, gets the map; every other
   shadow-casting light keeps its rays, and the reference renderer stays
   the ray-traced ground truth. A device without depth clamping or clip
   distances has no map (`nph_renderer_virtual_shadows_supported`), nor
   does a renderer created with `disable_virtual_shadows`; its sun is then
   ray traced, as is the sun of a view with `NPH_RENDER_VIS_RT_SHADOWS`
   (the `r.virtual_shadows` cvar).
2. **Light space and the clipmap.** Light space has axes u and v across
   the light and d along it, its origin at an anchor within 2 km of the
   camera, which moves to the camera (dropping every page) when the camera
   leaves that radius, so 32-bit coordinates hold anywhere in a 64-bit
   world (§8.2). Sixteen levels share it: level 0's window is 4 m across,
   each level's twice the one before (131 km for the sixteenth), each a
   window of 128 × 128 pages of 128 × 128 texels around the camera, so a
   level-0 texel is 0.24 mm. A level's frame is fixed to its own page grid
   and depth reference, both positioned in double precision on the CPU, so
   a page's contents stay valid while the camera moves; a level whose
   depth reference moves (by a quarter of its range) is dropped.
3. **Pages.** A level's page table is toroidal: entry (x mod 128, y mod
   128) holds absolute page (x, y), so a page keeps its entry while the
   window scrolls over it, and `vsm update` drops the entries the window
   left. Physical pages live in one D32 pool, 64 pages to a row
   (`vsm_pages`, 2,048 by default: 134 MB). Every frame, `vsm update` keeps
   the entries this frame requests (drawing them again if dirty) and asks
   a page for the others; `vsm collect` lists the free pool pages, then the
   evictable ones (owned, not requested), those unused for 60 frames before
   the recently used (a two-bucket approximation of least recently used);
   `vsm alloc` hands them out.
4. **Requests from the depth.** The depth pyramid's mip-0 passes (`vis hzb
   0`, and `vis hzb 1` in the tiles phase 1 drew into) already read every
   pixel's depth; they rebuild its point in light space with one matrix
   (`light_from_clip`) and set the pages its shading may read: the level
   whose texels match the pixel's footprint (at most twice it, Lefohn et
   al.), or the next whose window holds the point with a margin, and there
   the page and any other within 2 texels of it (the shading's offset along
   the normal, and slack), at most 2 × 2. Mostly a subgroup's pixels all
   need one page, which one invocation sets; otherwise, round after round,
   the lowest page any invocation still needs is set by one of those that
   need it. Blended surfaces are not in the visibility buffer, so where a
   frame has them, `vsm requests (blended)` rasterises them against the
   opaque depth (`vis depth`, now declared before it) and each fragment in
   front sets its pages.
5. **Caching and invalidation.** A drawn page stays drawn from frame to
   frame. An instance update marks dirty the pages its old and its new
   bounding sphere span in every level, a row of the rectangle at a time
   (a shadow map's texel holds the nearest caster along the light, so a
   caster changes exactly the texels under it); a dirty page is drawn
   again when next requested. The sun turning, the anchor moving, or a
   production frame without the map drops every page.
6. **Drawing.** `vsm cull instances` tests every instance against the
   pages drawn this frame at each level that has some (an indirect
   dispatch, so a frame that keeps every page culls nothing), handing on
   its meshlets; `vsm cull meshlets` makes a draw per (meshlet, page) it
   overlaps. Both scan only the rows that hold a page to draw (a per-level
   row summary). `vsm raster` clears the drawn pages with a quad each,
   then draws the casters with mesh shaders (indexed indirect draws
   elsewhere) into the pool, both faces, depth clamped (a caster nearer
   the light than the level's range still shadows), four clip distances
   confining each draw to its page. No fragment stage.
7. **Shading.** The resolve and the blended surfaces look the sun's
   visibility up once per point, before the light loop: the
   resolution-matched level, biased by 0.001 octaves to the finer side so
   that it reads the level the request took or the one finer, and up to
   three levels whose windows hold the point until one holds its page
   (lit past those). The point moves off its surface along its geometric
   normal (turned to the light) by 1.5 of the level's texels (normal-offset
   shadows; Holbert, "Saying Goodbye to Shadow Acne", GDC 2011). One
   gather fetches the four depths around it, each compared in the shader
   and weighted as bilinear filtering would: 2 × 2 percentage-closer
   filtering (Reeves, Salesin & Cook, SIGGRAPH 1987), its footprint kept
   inside the page. A depth-comparison sampler would do the comparison in
   the texture unit, but GLSL reaches it only by constructing a shadow
   sampler over an image of the bindless array, which SPIR-V types as a
   non-depth image; NVIDIA accepts that, and the gather is the portable
   form at the same cost (1.165 against 1.162 ms of resolve). The page
   table is read for the entry's state alone: `vsm update` has dropped
   every entry whose page the window left.
8. **Overflow.** The pool, the meshlet items (`max_vsm_meshlets`, 2^20),
   and the draws (`max_vsm_draws`, 2^20) are fixed. What does not fit is
   dropped, flagged in `NphRenderFrameStats.vsm_overflow`, and logged once;
   pixels whose pages did not fit are lit, and a page whose draws did not
   all fit is drawn again the next frame. Each overflow at the current
   resolution bias raises it (every step halves the levels' resolution,
   up to 3), so the pages fit within a few frames; 120 frames in a row
   whose pages would fit a step finer within three quarters of the pool
   lower it again (`vsm_lod_bias`).
9. **Contact shadows are deferred.** The map is resolution-matched: its
   texels are at most twice a pixel's footprint and the normal offset is
   1.5 of them, so the detail screen-space contact shadows would add is a
   pixel or two at contacts. A screen-space march costs a share of the
   shadow line the map already uses up (below). They return with the local
   lights' shadows, where the maps will be coarser, or when the reference
   scenes show the need.

## Measurements

| Scene (4K field) | GPU frame | Shadows' share | Resolve | `vis hzb 0` | `vsm` passes |
|---|---|---|---|---|---|
| no shadows | 1.905 ms | | 0.901 ms | 0.143 ms | |
| the sun's map, nothing moving | 2.307 ms | 0.40 ms | 1.172 ms | 0.217 ms | 0.048 ms |
| the sun's map, a sphere moving every frame | 2.342 ms | 0.44 ms | 1.169 ms | 0.216 ms | 0.094 ms |
| the sun's map, the camera walking (1.4 m/s at 240 Hz) | 2.336 ms | 0.43 ms | 1.174 ms | 0.221 ms | 0.060 ms |
| the sun's rays, nothing moving | 2.323 ms | 0.42 ms | 1.324 ms | 0.141 ms | |
| the sun's rays, a sphere moving (ADR-0032) | 2.714 ms | 0.79 ms | 1.309 ms | | |

Across runs the frame times move by about 0.02 ms (the still map measured
0.38 to 0.40 ms over no shadows); the table is the run the budgets come
from (`render_perf-2026-10-18`).

- **Pages.** The field needs 946 pages (62 MB of the pool) from its 8.3
  million pixels. The still scene draws none after its first frame; the
  moving sphere redraws 20 pages a frame with 191 caster draws; the walking
  camera draws a page every few frames.
- **Where the time goes.** The requests add 0.074 ms to the depth
  pyramid's first pass, which reads the whole visibility buffer anyway
  (half of it the rounds of subgroup minimums at page and level edges, a
  quarter of the subgroups). The map's own passes take 0.048 ms in a
  still frame. The rest is the resolve's lookup: 0.27 ms. About 0.1 ms of
  that is the lookup's own work (a page-table read, a gather and four
  comparisons, the level search); the rest is registers: the resolve
  compiles to 64 registers and 6.4 KB of shared memory per workgroup with
  the lookup, against 64 and 0.8 KB without, and a variant that has the
  lookup but never runs it already costs 0.16 ms.
- **The first version** took 2.546 ms in the still field: two pages per
  pixel requested (its level's and the next coarser one's, in eight
  waterfalls per subgroup), every level searched, four texel fetches for
  the filter, inside the light loop. Its requests took 0.303 ms of the
  pyramid's pass against 0.135, its resolve 1.320 ms. A sphere moving
  added 0.18 ms more, because a caster spanning more than 64 pages of a
  level marked the whole level dirty.
- **The lookup's place, measured** (resolve in the still field, the
  production path without shadows at 0.889 ms in the same runs): before
  the light loop with the face normal, 1.152 ms (64 registers, 6.4 KB
  spilled); inside the loop, 1.320 ms; with the sun taken last in the
  loop, 1.331 ms (72, 6.9 KB); first thing in the resolve with a normal
  rebuilt from four neighbouring depths, 1.130 ms (72 registers, nothing
  spilled, but every pixel looks up, facing the sun or not, and it needs a
  false data dependence to keep the compiler from starting the decode
  first); the same with the triangle decoded twice for the face normal,
  1.215 ms. The first is kept: it is the simplest, shared with the blended
  surfaces, and within 0.02 ms of the best.
- **Agreement.** Against the rays it replaces, 98 % of pixels are within
  two levels in `test_vsm`'s scene; the golden `shadows` scene's production
  render scores a FLIP mean of 0.017 and a p99 of 0.127 against the
  reference's ray-traced baseline on the RTX 3060 Ti (within ADR-0019's
  0.03 and 0.15): the edges of the sun's shadows sit about a pixel nearer
  the casters, where the normal offset moves them.
- **Memory.** The pool (134 MB by default), the page tables (4 MB for
  16 × 128 × 128 entries), and a work buffer of 25 MB with mesh shaders
  (45 MB with indexed draws), most of it the meshlet items' and the draws'
  lists.

## Alternatives considered

- **The sun's rays only** (ADR-0032): no pages, no requests, exact; but a
  top-level rebuild whenever anything moves (0.32 ms over 102,401 slots,
  0.22 ms over the field's own), only on devices with ray queries.
- **Cascaded shadow maps** (Engel, "Cascaded Shadow Maps", *ShaderX5*,
  2006): drawn again every frame over everything in their frusta, at a
  resolution that matches the pixels only at the cascades' near edges.
  CLAUDE.md asks for cached virtual pages instead.
- **Requesting each pixel's level and the next coarser one,** so that the
  shading could fall back across a level boundary: twice the pages. The
  margin around the page and the shading's bias toward the finer level
  make the fallback unnecessary.
- **Four texel fetches and the weights in the shader:** one gather does
  the same in one instruction.
- **A depth-comparison sampler** (above): no faster here, and not portable
  over the bindless array's image type.
- **The other places for the lookup** (above).
- **Marking a whole level dirty for a large caster,** and scanning every
  row of a level for its pages to draw: a single moving sphere cost
  0.18 ms; rows marked word by word and a row summary bring it to
  0.03 ms.
- **Static and dynamic pages apart** (Unreal Engine 5 caches the static
  casters' depth separately and draws the moving ones over a copy every
  frame): a moving caster now redraws its pages with every caster in them.
  It is the next step if scenes with much motion show the cost.
- **Pages of 64 × 64 or 256 × 256 texels:** smaller pages fit the pixels'
  footprints more tightly but quadruple the table and the requests' work;
  larger ones waste pool on page edges. Not measured yet.

## Consequences

- The production path's sun is shadowed on every device that has the
  production path, and its shadows cost the same whether things move or
  not: 0.38 to 0.40 ms in a still scene, 0.44 ms with motion, against
  the rays' 0.42 and 0.79 ms.
- The still scene's shadows sit at the §10.4 line (0.40 ms) and a scene
  with motion just over it (0.44 ms). The resolve's registers are the
  lever, for the map and for every later addition to the resolve
  (indirect light, stage 7): the next work on it is the resolve's
  register pressure as a whole.
- The map's DRAM traffic is small in a still frame: the requests' writes
  go to a 32 KB bitmask per level, the lookups read the pool pages under
  the pixels (62 MB held, sampled where needed, mostly from the caches).
  The declared footprints render_perf prints are upper bounds over whole
  buffers.
- CI's lavapipe (Mesa 25.2.8) drew none of the map's pages at first, and
  every pixel read as shadowed: a plain indirect draw (the pages' clears)
  read its count from the count buffer of an earlier indirect-count draw
  in the command buffer (the visibility buffer's raster), drew nothing,
  and the casters then failed their depth test against the uncleared
  pool. The RHI works around it for every caller
  (`NPH_GPU_QUIRK_INDIRECT_DRAW_STALE_COUNT`, docs/gpu.md), and a test
  draws the map through the indexed indirect path that lavapipe takes on
  every device.
- New: `NphRendererDesc.disable_virtual_shadows`, `vsm_pages`,
  `max_vsm_meshlets`, `max_vsm_draws`; `NPH_RENDER_VIS_RT_SHADOWS`;
  `nph_renderer_virtual_shadows_supported`; statistics `vsm`,
  `vsm_lod_bias`, `vsm_requested`, `vsm_drawn`, `vsm_resident`,
  `vsm_evicted`, `vsm_draws`, `vsm_overflow`; the runtime's
  `r.virtual_shadows` cvar; render-graph passes `vsm clear`, `vsm
  invalidate`, `vsm requests (blended)`, `vsm update`, `vsm collect`, `vsm
  alloc`, `vsm cull instances`, `vsm cull meshlets`, `vsm fixup`, `vsm
  raster`, `vsm statistics`.
- Open: contact shadows (above); local lights' shadows within budget;
  static and dynamic pages apart; filtering across page borders (a sample
  within half a texel of a page's edge is clamped inside it); smoother
  transitions between levels, whose texel size doubles at a distance
  boundary.

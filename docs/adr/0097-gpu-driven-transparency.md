# 0097. GPU-driven transparency: the blended surfaces culled, ordered and drawn by the GPU

- **Date:** 2026-11-14
- **Status:** Accepted (built and measured; the user's words of 2026-11-14 that it answers: "Track down the CPU
  overrun then push the branch", and, of what the tracking found, "Build GPU-driven transparency"; accepted
  2026-11-14 once the user had walked Bistro with it in the scene viewer: "The glass looks right, commit it and
  re-base the budgets")

## Context

The production path leaves every opaque and alpha-tested surface to the GPU: the CPU writes a view and the
records that changed, and declares the same passes whatever the scene holds (ADR-0029; CLAUDE.md §8.6.3, §12
Phase 10: "constant CPU cost vs object count"). Blended surfaces were the exception. Each frame the CPU walked
the instances with a blended material, tested each against the view, wrote a draw record for each of their
blended submeshes in view, sorted them back to front, and recorded a draw call for each in two passes (the
pass that asks for their pages of the sun's shadow map, and the pass that draws them).

That cost nothing while scenes held a handful. Since ADR-0094 Bistro's glass is blended and seen through: 342
instances, of which 279 are in the street's view. `nph_render_frame` took 0.090 ms there, 0.075 ms on the
terrace and 0.039 ms at the hedge (16 in view), against 0.022 ms for a view without any, and three of Bistro's
rows stood past the budgets' flat 0.050 ms. Timed, an instance on the list cost 0.044 µs a frame and a draw in
view 0.35 µs (docs/benchmarks.md "Reference scenes"). The first game is a city (ADR-0067): its windows would
not fit a main thread that has 1.0 ms for everything (§10.5).

Two conditions bound what may be done. The picture must not change ("We can not sacrifice any image quality to
get to the gate"). And the frame's gate is a 99th percentile on the GPU (ADR-0082), which two of the eighteen
reference views miss: the CPU's time must not be bought with the GPU's.

## Decision

1. **A table of items.** The renderer lists each blended submesh of each instance as an item
   (`NphVisBlendItem`: the instance, its mesh, the submesh's material slot and its indices), an instance's
   items a range of a GPU table (`RenderInstance.blend_range`, `NphRendererDesc.max_blend_items`). The table is
   written where instances are made and destroyed, and every instance is listed afresh when a material begins
   or ends to blend, gains or loses a side while it blends, or is destroyed while it blends
   (`nph_render_material_update` no longer does so for any other change). It is mirrored on the CPU and sent
   in chunks of 64 items, those that changed, with a production frame's other uploads. A frame that changes
   nothing does nothing for it.
2. **One index buffer.** Each mesh has an index buffer of its own, and one indirect draw binds one. A blended
   submesh's indices are therefore held in a pool (`blend_index_pool`), once whatever its instances
   (`RenderMesh.blend_indices`: a list's range, its users), written there by a compute pass of the next
   production frame (`vis blend indices`, a dispatch a list) and leaving with the last instance that draws it.
3. **The cull** (`vis blend cull`, `vis_blend.comp`), an invocation an item, after the frame's rasters and its
   depth pyramid. An item is a draw of the frame when
   - its instance's record is shown and is the one it was listed for (the same mesh, and for a one-sided item
     the same handedness), and the material its slot names there blends: a record reaches the GPU a frame late
     where a frame's updates are more than `max_instance_updates`, and a slot may have changed hands by then;
   - its mesh's box meets the frustum (`vis_box_in_frustum`, the world partition's cells' test; the box the
     positions dequantise into, carried through the transform after Arvo 1990);
   - its mesh's sphere is not behind the frame's opaque surfaces everywhere it covers (`vis_hzb_occluded`
     against the pyramid this frame's rasters left, as phase 1 of the instance cull tests the opaque instances;
     Karis et al. 2021). A blended fragment behind an opaque surface writes nothing and asks for nothing, so a
     surface wholly behind them is not missed. `NPH_RENDER_VIS_NO_OCCLUSION` switches the test off with the
     opaque ones'.

   For a draw it writes the record the forward shaders read (`NphRenderDrawData`, as the reference renderer's
   CPU writes one: the camera-relative transform from the split translation, the normals' matrix, the mesh's
   streams, the material's index, and whether it has one side) and its key, into lists it reserves a subgroup
   at a time, and raises the frame's count of draws (`NphVisArgs.blend_args`).
4. **The order is the reference renderer's.** The key is the view depth of the mesh's bounds' centre, its
   float's bits made to ascend and inverted, so that the farthest has the least key (`vis_blend_key`;
   `nph_render_ref_vis_blend_key` is the same function on the CPU, and the reference renderer's sort now calls
   it). Draws of one key fall in the order of their items: an instance's submeshes share a key and keep their
   mesh's order, as they did.
5. **The sort counts** (`vis blend sort`). A draw's place is the number of draws before it (comparison
   counting; Knuth, *The Art of Computer Programming* vol. 3, §5.2). A workgroup takes a draw, its 64 lanes
   each count a share of the keys, and one lane writes the draw's indirect command at the place they add up to.
   It is exact, needs one dispatch, and its workgroups are as many as the cull counted (an indirect dispatch
   from the same word the draws take as their count).
6. **One indirect draw a pass.** `vsm requests (blended)` and `transparent` each record one
   `vkCmdDrawIndexedIndirectCount` of the sorted commands, with one pipeline. The vertex stage finds its draw's
   record by the instance index (`forward.vert` `k_indirect`, specialisation constant 4), and hands the
   fragment stage what it needs of it (the material, the mirroring, the sides) so that no fragment reads the
   record.
7. **One pipeline, and the lists wound for it.** Draws of one-sided and two-sided materials, of mirroring
   instances and others, lie in one order and cannot be split by pipeline; and a pipeline that culls nothing
   rasterises every back face for the fragment stage to throw away (Alternatives). So the pipeline culls back
   faces, counter-clockwise the front, and a list in the pool is wound for that culling
   (`NPH_VIS_BLEND_AS_MADE`, `_TURNED`, `_BOTH`; a submesh has a list for each winding its instances need):
   - a one-sided submesh's triangles as the mesh winds them;
   - for an instance that mirrors, each triangle turned (its second and third corners exchanged), since a
     mirroring transform turns the windings on screen;
   - a two-sided submesh's triangles each followed by the same turned. The rasteriser draws the one of the two
     that winds counter-clockwise on screen (the areas it takes are exact opposites, and a triangle of no area
     draws nothing either way), so every triangle is drawn once, in the mesh's order, as with no culling. An
     even primitive is the triangle as made: the fragment stage reads the side from `gl_PrimitiveID`
     (`forward_blend_indirect.frag`, a variant of its own, since the reference renderer's shaders must not ask
     for the device feature that needs).

   The rasteriser thus leaves of each draw what a pipeline of its own left. An instance whose transform begins
   or ceases to mirror is listed afresh.
8. **The statistics are read back.** On the production path `draws`, `blended`, `dropped` and `triangles` are
   the GPU's counts of the blended surfaces, of the frame `vis_frame`, with `vis_blended_culled` (out of view)
   and `vis_blended_occluded` (hidden) beside them. A frame's draws past `max_blend_draws` are dropped and set
   `NPH_RENDER_VIS_OVERFLOW_BLEND`; an instance whose items do not fit the table, or whose lists do not fit
   the pool, is drawn without its blended submeshes, logged once.

The reference renderer is as it was: it culls, sorts and draws on the CPU, and is what the tests hold the
production path's order and picture to.

## Measured

On the RTX 3060 Ti at 2560 × 1440, a process a scene, 240 frames, each run started with the desktop still
(docs/benchmarks.md "The desktop shares the GPU too"); the build before (eb3b5f5) and after, run in turn,
two rounds of each, the mean of the two. The CPU's time is the median of `nph_render_frame`; the blended
passes' the sum of their means (the cull and the sort, the shadow pages' requests, the transparent pass);
the frame's its mean and its 99th percentile, the gate's (ADR-0082). Every reference view after, the first of
the two rounds against the budgets (below):
`benchmarks/results/reference_scenes-2026-11-14-adr-0097-win-msvc-profile-rtx3060ti.json`.

| View | CPU (µs) | Blended passes (µs) | Frame (ms) | 99th percentile (ms) |
|---|---|---|---|---|
| Bistro street | 89 → 22 | 288 → 262 | 5.94 → 5.94 | 6.05 → 6.03 |
| Bistro street walk | 88 → 23 | 296 → 275 | 6.12 → 6.12 | 6.32 → 6.35 |
| Bistro street turn | 42 → 23 | 221 → 227 | 6.24 → 6.27 | 7.32 → 7.34 |
| Bistro terrace | 74 → 23 | 250 → 228 | 6.65 → 6.66 | 6.77 → 6.77 |
| Bistro hedge | 39 → 23 | 155 → 168 | 5.63 → 5.66 | 5.74 → 5.76 |
| Interior floor | 22 → 21 | 224 → 229 | 4.63 → 4.62 | 4.68 → 4.67 |
| Interior floor walk | 23 → 21 | 207 → 215 | 4.68 → 4.67 | 4.72 → 4.72 |
| Interior floor turn | 22 → 21 | 302 → 315 | 4.33 → 4.35 | 4.70 → 4.68 |
| Interior gallery | 22 → 21 | 346 → 358 | 4.86 → 4.86 | 4.89 → 4.90 |
| Interior among | 22 → 21 | 324 → 331 | 4.77 → 4.77 | 4.81 → 4.81 |
| Interior upward | 21 → 21 | 263 → 274 | 4.28 → 4.29 | 4.32 → 4.34 |

- **The CPU.** Every view of Bistro takes what a view without glass took, and the interior's a microsecond
  less. The city has no blended surface and draws none of the new passes: its seven views are as they were
  (against the budgets' two rounds of the same day before it, the frames' means within 0.03 ms and their
  99th percentiles within 0.04, the CPU 24.6 µs against 24.9, within the rounds' spread).
- **The GPU.** The frames and their 99th percentiles are the same within the rounds' spread (the two rounds of
  one build differ by up to 0.10 ms in a frame's mean). The blended passes: the cull takes 8 to 14 µs and the
  sort 4 to 5 µs in these views; in Bistro's street the pyramid's test leaves 88 of the 279 draws in view,
  the requests go from 61 to 46 µs and the transparent pass from 227 to 198 µs, 0.021 to 0.026 ms less in
  all on the street, its walk and the terrace. Where none of the glass in view is hidden the two passes are
  added and nothing saved: Bistro's hedge and street turn and the interior's views, 0.005 to 0.014 ms more.
- **The budgets.** Two rounds of every reference view and synthetic scene against `reference_budgets.txt` and
  `render_budgets.txt` exceed no row: Bistro's three CPU rows, past the flat 0.050 ms since ADR-0094, are
  inside it again.
- **The glass wall** (`render_perf`'s `glass_*` scenes alone, 512 × 512, two rounds each; after:
  `benchmarks/results/render_perf-2026-11-14-adr-0097-glass-win-msvc-profile-rtx3060ti.json`):

  | Scene | CPU (µs) | Frame (ms) | Cull and sort (µs) | Transparent (µs) |
  |---|---|---|---|---|
  | 256 panes | 52 → 15 | 0.556 → 0.563 | 8 + 5 | 94 → 94 |
  | 1,024 panes | 195 → 15 | 0.580 → 0.588 | 8 + 7 | 135 → 132 |
  | 4,096 panes | 812 → 15 | 0.643 → 0.705 | 11 + 33 | 221 → 241 |
  | 1,024 panes, the sun's shadows | 196 → 16 | 0.686 → 0.687 | 8 + 7 | 137 → 134 |

  The CPU's 15 µs is a frame's whatever the panes. The 4,096 panes' 0.062 ms more on the GPU is the sort's
  0.033 (its count grows with the square of the draws), the cull's 0.011, and 0.020 in the transparent
  pass, where each of the 4,096 small draws is now an indirect one and each two-sided triangle reaches the
  rasteriser twice. With the sun's shadows the requests take 26 → 22 µs.
- **The picture.** The last frame of each view at an equal count of frames (render_perf's probe), before
  against after, against two runs of one build. With the radiance cache off, which is what makes a frame
  differ from run to run, each build repeats itself (0.000 to 0.017 % of the pixels, nearly all by a step or
  two), and before against after Bistro's street, street turn and terrace differ in 0.006 to 0.017 % of their
  pixels, as two runs of one build do; the interior's floor turn in 0.09 %, by one or two steps. With the
  radiance cache on, every view differs from the build before by about as much as two runs of one build do
  (Bistro's street in 0.85 to 0.95 % of its pixels, two runs 0.96 to 0.97 %), but for two turning views, in
  each of which a patch at the screen's edge comes out in one of several states from run to run with either
  build. Bistro's street turn: a strip on its left edge in one of two, each build seen in both. The
  interior's floor turn: a patch low on its left in one of three; six runs of each build alone put the build
  before in two of them (five runs and one) and the build after in two (four and two), the commonest the
  same, and with the cache off the two builds draw it alike.

## Alternatives considered

- **The cheap wins on the CPU** (a faster sort than `qsort`, the draw's index in the draw and not in push
  constants, cached bounds and normal matrices): about a third of 0.35 µs a draw. 279 draws would still not
  fit the rule, and the cost would still grow with the glass.
- **A budget for glass on the main thread.** It would have made Bistro's rows pass and left the city to find
  the same wall.
- **Indices pulled in the vertex stage** (a non-indexed draw whose vertex stage reads the mesh's indices by
  address): no pool, and built first. Every corner of every triangle is then shaded, where the hardware shades
  a shared vertex once: Bistro's street has 150,000 blended triangles (its glasses and bottles are dense), and
  `transparent` took 0.257 ms against 0.226 ms. A wall of two-triangle panes costs the same either way.
- **No culling, and the back faces ended in the fragment stage** (the pipeline culls nothing; the fragment
  stage turns a mirrored draw's facing back and discards a one-sided material's back faces first thing). Exact,
  simple, and built second. The interior's glass is one-sided with its back faces in view: `transparent` went
  from 0.342 to 0.396 ms in its gallery, half of what shading those fragments outright costs (0.453 ms), since
  a fragment thrown away has been rasterised and started all the same.
- **The sure back faces ended in the vertex stage**, with pulled indices (each vertex reads its triangle's
  three corners; a triangle whose area in pixels is on the back's side by more than the rasteriser's snapping
  can take from it goes to a point). Exact, and it took back 5 of the 30 µs the pulled indices cost Bistro.
- **The sort an invocation a draw**, each counting every key: 11 µs for 256 draws, 45 for 1,024 and 172 for
  4,096, growing with the count and not its square, since a lane waits on memory for each key it reads and the
  GPU runs far more lanes than there are draws. A workgroup a draw puts those lanes to work: 5, 7 and 35 µs.
- **A radix sort, or buckets of depth first.** Several dispatches each, a few microseconds of fixed cost
  apiece, to order a few hundred draws that one dispatch orders in 5 µs. Buckets are what a scene of more than
  some thousands of blended draws in view will want (Consequences).
- **A two-sided surface as two one-sided draws** (its list and its list turned), which needs no doubled list
  and no primitive index: all its front faces would then be drawn before all its back faces, where they were
  drawn in the order of its triangles. Another picture.
- **One index pool for every mesh's indices**, of which this pool would be a part: the usual shape of a
  GPU-driven renderer, and a change to every path that binds or builds from a mesh's own index buffer (the
  reference renderer, the bottom levels, the hit shaders) and to how scenes are sized. Not needed for this.
- **Mesh shaders for the blended draws**, which could cull a triangle's back face before the raster: another
  pipeline, a list of meshlets a draw, and a path beside it for devices without them.
- **Order-independent transparency** (weighted blended: McGuire & Bavoil, "Weighted Blended Order-Independent
  Transparency", JCGT 2013; lists a pixel: Yang et al., "Real-Time Concurrent Linked List Construction on the
  GPU", EGSR 2010). No order to compute, and another picture: not now.
- **A cell's glass left out where its proxy stands for the cell.** The CPU's path drew it, proxy or not, and
  so does this one; leaving it out changes what far cells show.

## Consequences

- **The CPU's cost of a frame no longer knows the glass.** Bistro's rows are inside the flat 0.050 ms again,
  and `render_perf` has a wall of glass (`glass_{256,1k,4k}_production`) to hold it there: 0.015 ms for 256
  panes and for 4,096.
- **Hidden glass is free of the passes that draw it.** The CPU could test against the frustum alone; the GPU
  has the frame's depth. The glassware of a room behind a wall no longer costs its vertices twice.
- **Two small passes while a scene lists any blended surface**, 0.012 to 0.019 ms together, and the two
  passes that draw them even when none is in view (they draw nothing). A view with blended surfaces in view
  and none hidden pays those passes and saves nothing: Bistro's hedge and street turn, the interior's views
  (0.005 to 0.014 ms). Both are latency, a few workgroups each; overlapping them with other work (the async
  queue, or a cull against the first phase's pyramid during the second) is what would take them back.
- **The sort grows with the square of the draws in view.** 33 µs at 4,096, which is `max_blend_draws`'s
  default; the description takes no more than 16,384. Past some thousands a scene wants its draws put into
  buckets of depth first and ordered within them, which is exact too and three dispatches more.
- **Many small draws cost the GPU more as one indirect draw.** The glass wall's 4,096 two-sided panes take
  the transparent pass 0.020 ms more than they did as 4,096 of the CPU's draws. Runs of one submesh in the
  order could be drawn instanced (the vertex stage finding its record through the order), which the order
  allows: a city's windows share their meshes.
- **The pass that asks for shadow pages culls as the pass that shades.** It drew both faces of everything
  (one pipeline had to serve every draw): a one-sided surface's back faces asked for pages that nothing shaded
  from. They no longer ask.
- **Memory.** 24 bytes an item on the GPU and as many in its mirror, 8 a list waiting (65,536 items by
  default: 3.7 MB in all), 188 bytes a draw a frame may make (4,096: 0.8 MB), and the index pool (2^21
  indices: 8 MB), in which a two-sided submesh's list is twice its indices and a submesh drawn with several
  windings has a list for each. DRAM traffic a frame: the items (24 bytes each), the records and keys of those
  in view (168 bytes each), and the keys again for each draw in view (8 bytes a pair of draws); under 0.1 MB
  for Bistro's street, where the passes' declared footprints count their whole buffers.
- **Not the same bits as the CPU's path.** A draw's translation is the GPU's difference of split floats
  (docs/render.md "The GPU scene"), as every opaque instance's is, where the CPU rounded a difference of
  doubles once: an ulp apart at most. A mirrored one-sided surface's triangles, and a two-sided surface's back
  faces, reach the rasteriser with two corners exchanged, which may round an interpolated value's last bit
  another way. And two different instances whose keys are equal to the bit fall in the order of their items in
  the table, where they fell in the order of the CPU's list. The pictures differ no more than two runs of
  one build do: with the radiance cache off, in 0.006 to 0.017 % of Bistro's pixels and 0.09 % of the
  interior's turning view's, by a step or two (Measured).
- **The frame's statistics of blended draws are two frames old**, as its other GPU counters are.
  `max_draws` now bounds the reference renderer's draws alone.
- **A material that begins or ends to blend, or gains or loses a side while it blends, costs a walk of every
  instance**, once. Its other changes no longer cost one.
- **Blended surfaces are still ordered whole**, by their bounds' centres: crossing surfaces, and a surface
  seen through itself, blend in the order of their draws and triangles. That was so, and is not this ADR's to
  change while the picture may not.

## Tests

- `engine/render/tests/test_vis_blend.c`: 301 draws at as many depths, made in no order, read back: every one
  once, farthest first, a two-submesh instance's halves in their mesh's order, each key the CPU's for its
  depth, the pool's list each triangle and its turn; 24 panes that overlap in threes against the reference
  renderer's picture (and against the same panes at mirrored depths, which is another picture); what the cull
  keeps of panes in view, out of view, hidden and opaque, and after they are hidden, shown and moved; glass
  behind a wall not drawn, the picture the same to the bit with the test off; one-sided and two-sided panes,
  toward and away, mirrored or not, against the reference renderer, the two that show their backs not drawn;
  the three windings' lists read back against the mesh's indices, an instance that begins to mirror and a
  material that gains a side wound afresh, each against the reference renderer; the table through instances
  and materials that come, go, begin and end to blend; items written by hand that name an opaque instance,
  another mesh and a destroyed instance, none drawn; a population of 200 replaced three times; the index pool
  holding a list once whatever its instances, emptied and filled again; draws past `max_blend_draws` dropped
  and said, the rest in order; instances past `max_blend_items`, and past the pool, drawn without their
  blended submeshes and said, and drawn once there is room.
- `Vis.culling_math_matches_its_cpu_twins`: `vis_box_in_frustum` against `nph_render_ref_vis_box_margin` and
  `vis_blend_key` against `nph_render_ref_vis_blend_key`, bit for bit, over 8,192 cases (both zeros, either
  side of the camera's plane, twelve decades).
- The tests that held the CPU's path hold this one: `Vis.alpha_modes_and_sides_match_the_reference`,
  `Vis.blended_surfaces_behind_opaque_ones_match_the_reference` (whose hidden pane is now not drawn),
  `Vis.glass_shows_what_is_behind_it…`, `Vsm.a_blended_surface_is_shadowed`, and the many lights' and
  reflections' blended surfaces.
- `render_perf`: `glass_1k_reference`, `glass_{256,1k,4k}_production`, `glass_1k_shadows`.

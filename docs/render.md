# Render module (`engine/render`)

Two renderers behind one API (CLAUDE.md §8.6), chosen per view by
`NphRenderView.pipeline`, plus the pieces both use: image-based lighting,
tonemapping, the debug-draw API, the GPU profiler, procedural primitives,
and the meshlet builder.

- The forward PBR **reference renderer** (stages 1–2,
  [ADR-0018](adr/0018-reference-renderer.md)) is the **ground truth**. It
  draws each visible submesh with its own draw call and evaluates every
  light at every pixel; its CPU cost grows with the object count and its
  GPU cost with lights × pixels. It favours exactness over speed.
- The GPU-driven **production path** (stages 3–5,
  [ADR-0029](adr/0029-gpu-driven-visibility-buffer.md),
  [ADR-0030](adr/0030-hybrid-raster-and-meshlet-work-lists.md),
  [ADR-0031](adr/0031-light-grid-and-compact-hdr.md); "The production
  path" below) culls, rasterises into a visibility buffer, bins its lights,
  and shades on the GPU. Its CPU cost depends on neither the object count
  nor the light count. Its images are tested against the reference's.

Both draw shadows for the lights that cast them ("Shadows" below): the
production path's sun from its virtual shadow map
([ADR-0033](adr/0033-virtual-shadow-maps.md)), every other light with
shadow rays where the device has ray queries
([ADR-0032](adr/0032-ray-traced-shadows.md)). The production path
anti-aliases temporally at native resolution where the view asks
("Temporal anti-aliasing" below; [ADR-0034](adr/0034-temporal-anti-aliasing.md))
and blurs motion over the view's shutter time ("Motion blur" below;
[ADR-0036](adr/0036-motion-blur.md)). Both see through a thin lens where
the view has an aperture ("Depth of field" below;
[ADR-0037](adr/0037-depth-of-field.md)), and add bloom and expose
automatically where it asks ("Bloom and exposure" below;
[ADR-0035](adr/0035-bloom-and-exposure.md)).

Every formula is cited in the shader that implements it, and has a CPU
twin that the tests check it against.

| Header | What it holds |
|---|---|
| `renderer.h` | the renderer: textures, meshes, materials, instances, lights, the environment, frames |
| `debug_draw.h` | immediate-mode debug lines (depth-tested and overlay) |
| `gpu_profiler.h` | per-pass GPU times and declared bytes over a rolling window |
| `primitives.h` | cube, sphere, plane, cylinder in the cooked vertex streams |
| `meshlet.h` | the meshlet builder (moved from the cooker, which wraps it) and meshlet bounds |
| `reference.h` | CPU reference implementations of the shader math, the culling tests included |

Shaders live in `shaders/render/`. The records they share with C are in
`shaders/shared/nph_render.h` and, for the production path,
`shaders/shared/nph_vis.h`.

## Units and conventions

- **Space.** Right-handed, Y up, metres, column vectors (ADR-0008).
  Positions are 64-bit (`NphDVec3`) on the CPU: instance translations,
  light positions, the camera, and the debug-draw origin. Each frame
  subtracts the camera position in double precision before rounding to
  32 bits, so the GPU only ever sees camera-relative coordinates (§8.2).
  A test renders the same image 10,000 km from the origin.
- **Projection.** Reverse-Z with an infinite far plane (Reed 2015): depth
  is cleared to 0 and tested with GREATER.
- **Light.** Punctual lights use the units of `KHR_lights_punctual`:
  directional lights in lux, point and spot lights in candela. Spot cones
  attenuate as the extension specifies, and point and spot lights fall off
  with the inverse square, windowed to zero at their range (Karis 2013).
  Environment radiance and emissive radiance are in cd/m².
- **Exposure.** Each view sets EV100. Radiance is multiplied by
  `1 / (1.2 · 2^EV100)` (Lagarde & de Rousiers 2014, §5.1) *before* it is
  written. The HDR target therefore holds pre-exposed values, and sunlit
  GGX highlights (whose peak can exceed 10⁵ cd/m²) stay inside half-float
  range. The forward pass also clamps to 65504.

## Resources

All resources are 32-bit generational handles, so a stale handle is
detected and refused rather than dereferenced.

| Resource | Lives | Notes |
|---|---|---|
| texture | GPU, immutable | levels uploaded at creation through the staging ring; any sampled format, BC included; 2D or cube |
| mesh | GPU, immutable | the cooked streams as they are: unorm16 positions in the bounds, octahedral normals, snorm8 tangents, half UVs, RGBA8 colours; up to 256 submeshes; with ray tracing, a bottom-level acceleration structure over the positions |
| material | CPU record | metallic-roughness glTF material with five texture bindings, each with its own wrap and filter |
| instance | CPU record | a mesh, a material per material slot, a 64-bit transform, hidden flag |
| light | CPU record | directional, point, or spot; casts shadows or not |

Materials, instances, and lights are copied into per-frame upload buffers
(one per frame in flight) when a frame is declared. Editing them therefore
never races a frame the GPU is still reading. Materials are re-copied into a
slot only when they changed since that slot was last filled, because each
material carries a version counter.

`render` sits left of `asset` in the module order (CLAUDE.md §4), so it
cannot read cooked assets itself. `samples/common/cooked_scene.c` is the
bridge: it loads a cooked scene and its dependencies through the asset
loader and creates the render resources. The samples and the golden tests
share it, and the runtime module takes it over in Phase 9.

## A frame

`nph_render_frame(renderer, view, graph, target, &rg_target, &stats)` is
called inside a GPU frame, after `nph_gpu_rg_begin`. It fills the frame's
records and declares passes into the caller's render graph. The caller may
then add its own passes (a UI, a capture, the present), compile, and execute.

1. **Records.** The view (matrices, exposure, bindless indices of the
   lighting tables), the lights, the materials (if changed), and one draw
   record per visible submesh. Instances are frustum-culled on the CPU
   against their mesh bounds. Each draw record carries the camera-relative
   affine transform, its inverse transpose for normals, the mesh's
   dequantisation, and the stream addresses.
2. **Sorting.** Opaque and masked draws sort by pipeline (alpha mode,
   culling), then by mesh. Blended draws sort back to front by the view depth of their
   bounds' centre. Ties break on the record index, so the order is
   deterministic.
3. **Passes.**

| Pass | Type | Reads | Writes |
|---|---|---|---|
| `brdf lut` | compute, first frame only | | DFG table |
| `ibl source`, `ibl downsample` ×n, `ibl sh`, `ibl prefilter` ×m | compute, when the environment changed | environment levels | environment chain, SH, prefiltered levels |
| `rt accel` | compute, when a light casts shadows | the instance records | new meshes' bottom levels, the top level ("Shadows") |
| `forward` | graphics | lighting tables, SH, the top level when it traces | pre-exposed RGBA16F colour, D32 depth |
| `dof setup`, `dof tiles`, `dof gather`, `dof resolve` | compute; with an aperture | HDR colour and depth (sampled) | an HDR image with depth of field, which the passes after read ("Depth of field") |
| `exposure clear`, `bloom down 0` to `bloom down N`, `bloom up N - 1` to `bloom up 0`, `exposure average`, `exposure readback` | compute, transfer; with bloom or an automatic exposure | HDR colour (sampled) | the bloom chain, the metering ("Bloom and exposure") |
| `tonemap` | graphics | HDR colour (sampled), depth (read-only attachment), the bloom chain's mip 0 | the caller's target |

The forward pass draws, in order: the opaque and masked draws, the sky
(a fullscreen triangle at depth 0, so GREATER_OR_EQUAL passes only where
nothing was drawn), then the blended draws over both. The tonemap pass
draws a fullscreen triangle and then the debug lines over the result.
Depth-tested lines are tested against the scene depth; overlay lines are
not.

Mirroring transforms (a negative determinant) turn counter-clockwise
fronts clockwise on screen. Such draws use a pipeline with
`front_face = CW` and flip the tangent frame's handedness, so normal maps
stay correct on mirrored copies. A test checks both the culling and the
lighting.

## Shading

`shaders/render/forward_frag.glsl` evaluates the metallic-roughness model
of glTF 2.0:

- **Specular:** GGX (Trowbridge-Reitz) distribution, Smith
  height-correlated visibility (Heitz 2014, in the form of Lagarde & de
  Rousiers 2014), and Schlick Fresnel with F0 = 0.04 for dielectrics.
  Perceptual roughness is clamped to 0.045, as Filament does.
- **Diffuse:** Lambert, with the base colour scaled by (1 − metallic).
- **Energy compensation:** punctual specular is scaled by
  `1 + F0 (1/Ess − 1)` (Kulla & Conty 2017). Image-based lighting uses the
  full multiple-scattering model of Fdez-Agüera 2019, so a white,
  non-metallic material under a uniform sky reflects exactly the sky's
  radiance (a test checks it).
- **Textures:** base colour and emissive in sRGB formats (the hardware
  decodes them), metallic in B and roughness in G, occlusion in R.
  Normal maps follow glTF's MikkTSpace frame in its normalised variant:
  the interpolated normal and tangent at unit length, and the bitangent
  `cross(n, t) · w`. The unnormalised variant assumes both arrive as
  interpolated unit vectors. A transform that scales breaks that: it
  stretches the tangent by its scale and shrinks the normal by it, and
  Bistro (scaled by 100) shaded in the surface's plane. The two variants
  differ only to second order in the frame's spread across a triangle.
  Two-channel maps (BC5, RG8) rebuild z. Meshes without tangents get a
  frame from screen-space derivatives (Schüler 2006).
- **Occlusion** applies to indirect light only (glTF), and also drives
  specular occlusion (Lagarde & de Rousiers 2014). On the production path
  the screen's ambient occlusion multiplies it, the diffuse light's with
  what the occluders reflect back ("Ambient occlusion"), and the diffuse
  light can take the radiance cache's irradiance instead of the
  environment's ("Global illumination").
- **Alpha:** MASK discards below the cutoff in its own shader variant (so
  the opaque variant keeps early depth testing). BLEND composites "over"
  without writing depth. Double-sided materials flip the whole tangent
  frame on back faces.
- **Unlit** materials (`KHR_materials_unlit`) write their colour as
  authored, outside exposure.
- **Debug views:** base colour, shading normal, roughness, metallic,
  occlusion, direct light only, indirect light only, and the diffuse
  irradiance the indirect light takes (over π, at the interpolated
  normal: the environment's, or the radiance cache's); on the production
  path also clusters, triangles, and instances (a hashed colour each).

`shaders/render/surface.glsl` holds this model; the forward pass and the
production path's resolve both include it, so the two renderers cannot
drift apart. The resolve has no rasteriser derivatives, so it supplies
its own (see below). The environment and emission are evaluated before
the punctual lights, so that only the point's description and one sum
stay live across the light loop, where the resolve's registers peak. The
reference lights every pixel with every light; the production path's
view carries a light grid instead ("Lights" below).

## Shadows

A light casts shadows when its description says so
(`NphRenderLightDesc.shadows`). Two techniques draw them:

- **The production path's sun**, the live directional light that casts
  shadows in the lowest light slot, is shadowed by its **virtual shadow
  map** (below; [ADR-0033](adr/0033-virtual-shadow-maps.md)) wherever the
  renderer has one: the device has depth clamping and clip distances, and
  `NphRendererDesc.disable_virtual_shadows` is clear
  (`nph_renderer_virtual_shadows_supported`).
  A view with `NPH_RENDER_VIS_RT_SHADOWS` (the runtime's
  `r.virtual_shadows` cvar off) traces the sun's rays instead.
- **Every other shadow-casting light**, and every light of the reference
  renderer (the ground truth), traces **shadow rays**, wherever the
  renderer has ray tracing: the device has `VK_KHR_ray_query` (the RTX
  3060 Ti, and lavapipe in CI) and `NphRendererDesc.disable_ray_tracing`
  is clear (`nph_renderer_ray_tracing_supported`). Elsewhere such lights
  draw unshadowed, logged once.
  [ADR-0032](adr/0032-ray-traced-shadows.md) records the decision and the
  measurements.

### Shadow rays

Where a shadow-casting light reaches a shaded point
(inside its range and cone, and facing it), the point traces one ray
toward it:

- A directional light's ray is unbounded; a point or spot light's ends at
  the light, so what lies beyond the light casts nothing.
- The first hit ends the query, and every triangle is opaque. A point is
  either lit or not, so the shadows are hard: punctual lights have no area.
- The ray leaves from the side of the triangle's geometric normal that
  faces the light. Its origin is pushed off the surface by 256 units in the
  last place of each coordinate (Wächter & Binder, *Ray Tracing Gems*,
  2019, chapter 6), so it never meets the triangle it starts on at any
  distance.

`surface.glsl` calls the test (`NPH_SURFACE_SHADOW`) from its light loop,
so the reference's forward pass and the production path's resolve and
blended surfaces trace the very same rays. Each has a variant compiled
with ray queries (`forward_*_rt.frag`, `vis_resolve_rt.comp`). A frame
takes those variants only while a light casts shadows that the sun's map
does not take; the resolve's compiles to 72 registers against 64. The
reference's shadows are the ground truth the virtual shadow map is tested
against.

**Acceleration structures** (`engine/render/src/rt.c`, over
`gpu/accel.h`):

- **A bottom level per mesh**, over its quantised positions as the cooked
  stream stores them (R16G16B16A16_UNORM, where the device builds from that
  format; a float copy of the same values elsewhere). It is built once, in
  the first frame that traces after the mesh's creation
  (`NphRenderFrameStats.shadow_builds`), preferring trace speed, and
  compacted after: its compacted size is queried in the pass that built
  it, read when that frame completes two frames on, and a structure of
  that size made; the next frame that traces copies it there and swaps it
  in (the reference's records name the copy from the next frame, the
  production path's once the GPU scene's mesh record, uploaded in the
  swap's frame, reaches the GPU), and the original is retired two frames
  after the swap, when no top level still to be traced names it. A device
  that saves nothing (lavapipe reports the size as built) keeps the
  structure as built. `shadow_blas_bytes` in the frame statistics is the
  bottom levels' size, `shadow_compactions` the swaps a frame made.
- **A top level over the instances.** Each record's transform folds in its
  mesh's dequantisation (`L·diag(scale)`, `L·offset + t`), and its custom
  index names its slot. The live, shown instances' records come first;
  hidden and free slots follow as inactive records (no structure, a zero
  mask) or are left out. Vulkan allows inactive records anywhere, but
  lavapipe (Mesa 25.2) encodes only as many leaves as are active, counted
  from the start, and would lose an active record behind an inactive one.
  - **Its space** is the world less an anchor near the camera, so 32-bit
    coordinates hold anywhere. A shader moves a view (camera-relative)
    point into it by adding `NphRenderViewData.tlas_offset`, the camera's
    position less the anchor.
  - **The production path** writes the records on the GPU from its GPU
    scene (`rt instances`: every slot's record inactive, then the active
    ones appended from the start), so its CPU cost does not depend on the
    instance count. It rebuilds the top level only when its instances
    changed, after a frame that traced nothing, when the camera has moved
    more than 16 m from the anchor, which then moves to the camera, and
    the frame after a swap of compacted bottom levels. A camera moving
    through a still scene costs no build.
  - **The reference** writes the active records on the CPU into its frame
    records, as it does its draws, and rebuilds every frame over them, with
    the anchor at the camera. `shadow_rebuild` in the frame statistics says
    when either path rebuilt.
- **One scratch buffer** serves every build. It is sized at creation for
  the top level of `max_instances` and grown at mesh creation when a
  bottom level needs more, so a frame never allocates. A frame's
  bottom-level builds fill it in batches with a barrier between them; the
  top level's build follows in the same pass (`rt accel`), after a barrier
  that also makes the bottom levels visible to the shaders that trace.

The top level for 119,041 slots (`render_perf`) takes 20.7 MB, its records
7.6 MB (and, for the reference, 7.6 MB more in each frame's upload
buffer), and the scratch 9.8 MB. Compacted, Bistro's 554 bottom levels (1.76 million triangles) hold
55.7 MB against 124.1 MB as built (70 bytes a triangle on the RTX 3060
Ti), and `lod_4k_production`'s 5.8 MB against 13.8. The rays trace them
as fast: interleaved, the radiance cache's update takes 0.192 ms against
0.197 in Bistro's street, 0.178 against 0.177 as it turns, and 0.037 in
both in the LOD scene; the resolve is unchanged and the frames within
0.02 ms. A swap costs one more top-level build, the frame after it.

### The sun's virtual shadow map

(`engine/render/src/vsm.c`, `shaders/render/vsm_*`, the records in
`shaders/shared/nph_vsm.h`; Fernando et al., "Adaptive Shadow Maps",
SIGGRAPH 2001; Lefohn, Sengupta & Owens, "Resolution-Matched Shadow Maps",
ACM TOG 2007; Epic Games, "Virtual Shadow Maps", 2022.) A shadow map of
the sun whose resolution matches the pixels that read it, held only in
the pages some pixel needs, and kept drawn from frame to frame until
something it shows changes.

**Layout.**

- **Light space**: axes u and v across the light, d along it, with its
  origin at an anchor within 2 km of the camera (which moves to the camera,
  dropping every page, when the camera leaves that radius), so 32-bit
  coordinates hold anywhere.
- **A clipmap of 16 levels** around the camera. Level 0's window is 4 m
  across and each level's twice the one before (131 km for the sixteenth);
  a window is 128 × 128 **pages** of 128 × 128 depth texels, so a level-0
  texel is 0.24 mm. A level's frame is fixed to its own page grid and
  depth reference (placed in double precision on the CPU), so a page's
  contents stay valid while the camera moves; a level whose depth
  reference moves (a quarter of its range) is dropped.
- **Page tables**, one per level, toroidal: entry (x mod 128, y mod 128)
  holds absolute page (x, y), so a page keeps its entry while the window
  scrolls over it.
- **The pool**: the pages that hold depth, in one D32 image of 64 pages a
  row (`vsm_pages`, 2,048 by default: 134 MB).

**A frame** (the production path, while it has a sun):

| Pass | What it does |
|---|---|
| `vsm clear` | the frame's counters and page bitmasks to zero |
| `vsm invalidate` | before the scatter: each instance update marks dirty the pages its old and its new bounding sphere span, in every level (an invocation per update and level), a row of words at a time |
| `vis hzb 0`, `vis hzb 1` | the depth pyramid's mip-0 passes **request** pages: each pixel's point rebuilt in light space from its depth (the view's rays), and the pages its shading may read set in the requests bitmask, a 2 × 2 block's at once where it can (`vsm_block.glsl`) |
| `vsm requests (blended)` | with blended surfaces, which the visibility buffer does not hold: the blended draws with no attachments, each fragment in front of the opaque depth (the visibility buffer's, tested in the shader) setting its pages (`vsm_request.frag`) |
| `vsm update` | every entry against this frame's windows: an entry whose page the window left gives its physical page up; a requested entry with a page keeps it (and is wanted, to be drawn again, if dirty), one without is wanted, to take a page; the wanted pages listed and counted per level |
| `vsm collect` | the free physical pages, then the evictable ones (owned, not requested: those unused for 60 frames before the recent ones) |
| `vsm select` | the wanted pages to draw this frame within the page budget, the coarse levels first ("Overflow and the page budget", below): a dirty page granted is drawn, a new one takes a free page, else an evictable one, and is drawn; the rest wait for a later frame |
| `vsm cull instances`, `vsm cull nodes`, `vsm cull meshlets` | for each level with pages to draw (an indirect dispatch: none in a frame that keeps every page), the instances whose bounding spheres reach such a page; a mesh with levels of detail walks its hierarchy against those pages, two node levels a step (below); then the meshlets: a draw per (meshlet, page), scanning only the rows that hold a page to draw |
| `vsm fixup` | where the draws did not all fit, the pages drawn are kept dirty, to be drawn again |
| `vsm raster` | the pages to draw cleared (a quad each), then the casters drawn into them, both faces, depth clamped, four clip distances confining each draw to its page; mesh shaders, or indexed indirect draws |
| `vsm voxel splat`, `vsm voxel merge` | while the renderer holds voxel meshes: the voxel clusters' casters splatted into a depth image of their own, a workgroup per (cluster, page), then taken into the pages drawn, whose depth test keeps the nearer of theirs and the triangles' ("Voxel casters", below) |
| `vsm statistics` | the counters, read back two frames later |

**Requests.** A pixel asks for the level whose texels match its footprint
(the texel at most twice the footprint) or, where that level's window
does not hold its point with a margin, the next that does (a narrow view
from afar), and there the page that holds the point and any other within
2 texels of it (the shading's offset along the normal, and slack): at
most 2 × 2 pages. The depth pyramid's invocations each read a 2 × 2 block
of pixels, and where the four lie on one surface at one level the block
asks at once: the rectangle around their points, widened by the margin,
takes the same level, window test and pages as each point's own (all
three only grow with a point's coordinates and distance), so where it
spans one page or two it holds exactly the four's pages (the pixel
nearest each end takes the page there). Near a page's corner (where the
rectangle spans four, one of which no pixel may need), where the four's
levels differ, where the background shows, and across an edge between
surfaces, each pixel asks for its own: the subgroup gathers those pixels,
four from each invocation that has them, into consecutive invocations
with shuffles and computes them together, where one invocation going
round its four would hold the subgroup up (shuffles, not shared memory: a
barrier in control flow some subgroups skip broke on lavapipe). Each
distinct request then goes to an invocation of its own, round after
round the lowest any invocation still has, and every invocation sets the
one it was given, their atomics together. A pixel's point comes from the
view's rays: the infinite reverse-Z projection makes it (ray_x x + ray_y y
+ ray_o) / depth for NDC x and y, fewer operations than the homogeneous
matrix the rays replace, and its level from the squared distance's
exponent, without a root or a logarithm. Every quantity the block and its
pixels compare goes through the same precise functions, so they agree to
the bit, and a test holds the blocks' requests to every pixel's own over
a depth image built to take every way through them (page edges and
corners, level boundaries, the background, edges between surfaces,
foliage-like noise, points beyond their level's window). In Bistro's
street the first pyramid pass went from 0.382 to 0.307 ms (278 to 175
instructions a pixel; it now runs at 73% of DRAM bandwidth), the second
from 0.045 to 0.042 ms, with the pages requested the same within the
frame's jitter.

**Caching.** A drawn page stays drawn. A caster's change dirties exactly
the pages under its bounding sphere, old and new, since a shadow map's
texel holds the nearest caster along the light; a dirty page is drawn
again (with every caster in it) when next requested, within the page
budget ("Overflow and the page budget", below). The sun turning, the
anchor moving, or a production frame without the map drops every page.

**Levels of detail.** A caster with levels of detail ("Levels of detail"
below) is drawn into a level's pages at one cut for the whole level, not
at the camera's: every texel of a level has the level's size, so one error
suits all of it, half a texel (`VSM_LOD_TEXELS`), which the receiver's
offset of 1.5 texels along its normal covers with a texel to spare. The
cut's threshold is the largest power of two within that error at the
instance's scale (`vsm_lod_threshold`): a cluster is in it when its
maker's error is at most the threshold and its consumer's more, level 0
where even the finest group is too coarse, the coarsest level where even
the root's are fine. `vsm cull instances` hands such a caster's hierarchy
to `vsm cull nodes` (`vsm_cull_nodes.comp`), a step down two of its node
levels at a time as the camera's walk takes them (`vis_cull_nodes.comp`):
a node or group is taken when a group below it is too coarse for the
threshold, a cluster below it fine enough, and its sphere reaches a page
of the level to draw; a group's clusters go to `vsm cull meshlets`, which
keeps those whose maker's error is fine enough and tests them as any
caster. So a page takes the cut's clusters near it, not the whole cut of
every instance whose sphere reaches it: a tree of a million triangles
beside a small page drawn hands on the clusters that reach the page. The
cut follows the instance's scale and the level alone, so a page drawn
stays right while the camera moves; an instance's change redraws its
pages as before, and turning the levels on or off (a threshold of 0)
drops every page. (The cuts were tables of clusters made with each mesh,
`nph_render_lod_cuts`, handed on whole: the landscape's pines, 289,000
clusters at level 0, overflowed the list with a few trees by the camera.)

With the sun moving every frame at a day-night cycle's pace
(`lod_shadows_sun_4k_production`: the terrain and 1,024 rocks of
`lod_4k_production`, every one of 750 pages drawn again each frame), the
cuts take the caster draws from 326,586 to 58,039 and the raster from 8.08
to 1.54 ms, the frame from 11.95 to 5.37 ms; the instance cull falls from
0.103 to 0.026 ms (fewer meshlets to hand on), and the meshlet cull, which
reads the entries, from 0.058 to 0.049 ms. Where pages stay drawn (the
camera walking in that scene, `lod_shadows_walk_4k_production`, and in
Bistro's street, cooked with levels) the map draws a page or a few a frame,
and nothing measurable changes: the culls within a microsecond, the frames
within the runs' spread (interleaved, -0.011 and +0.008 ms in Bistro). The
tables hold about 1.2 entries a cluster (367 for the test torus's 297).

**Voxel casters.** A mesh's cuts hold its voxel clusters too (ADR-0061,
stage 4), and `vsm cull meshlets` sends their draws to a list of their own
(`max_vsm_voxel_draws`, in a buffer made with the first voxel mesh, as the
casters' depths are). `vsm voxel splat` (`vsm_voxel_splat.comp`) takes a
workgroup a draw, as the visibility buffer's voxel raster takes a cluster:
each voxel projects its footprint's box to the page's texel centres it may
cover, from steps along its brick's grid in the level's frame, a subgroup
pools its voxels' texels, and each texel's ray, the sun's through its
centre from its brick's corner's light-space depth, is tested against that
voxel alone (`vis_voxel_hit`). The hit's depth takes the atomic minimum of
the texel in an R32 image laid out as the pool, which, a depth attachment,
takes no atomics. `vsm voxel merge` then draws the clear's quad over each
page drawn: a texel a voxel cast on takes its depth, which the depth test
keeps where it is nearer than the triangles', and every texel of the page
is reset for the next time. Each ray starts at its texel's centre in the
level's frame, exactly (an integer and a half texels from its page's
corner), so the map holds what the CPU's rays through the same clusters
meet: over a grid of 25 turned foliage meshes, every one of 2,325 texels,
its depth within 0.0005 of a texel. Against level 0 the voxels cover 7.8 %
fewer of the map's texels, and the floor below loses 6.3 % less light to
their shadow, where the stalled triangle levels the map drew before lost
10.6 % (`test_vsm.c`). With the sun moving every frame over `render_perf`'s
foliage (`foliage_shadows_sun_4k_production`, about 830 pages drawn again
each frame) the casters take 12.2 ms (the triangles' raster 7.6, the
splat 4.1, the merge 0.4) where the stalled triangles took 16.9, and the
frame 20.0 ms where it took 24.7; with the camera walking
(`foliage_shadows_walk_4k_production`) the map's passes are within a few
microseconds of before. Frames without voxel meshes bind the cull and the
page passes specialised without the voxels' routing and arguments
(`k_voxels`), and keep the work buffer as it was. The depths image is the
pool's size in R32 (134 MB for the default 2,048 pages), the list 16 MB.

**Shading.** The resolve and the blended surfaces look the sun's
visibility up once per point, before the light loop (inside it, the
lookup's registers would add to the loop's): the resolution-matched level,
biased by 0.001 octaves to the finer side, so that it reads the level the
request took or the one finer and passes to the next; up to three levels
whose windows hold the point, until one holds its page (lit past those).
The point moves off its surface along its geometric normal, turned to the
light, by 1.5 of the level's texels (normal-offset shadows; Holbert, GDC
2011), and one gather of the four depths around it, each compared and
weighted as bilinear filtering would, gives 2 × 2 percentage-closer
filtering, kept half a texel inside the page (a depth-comparison sampler
would need a depth image type the bindless array does not declare). Only an
entry's state word is read: `vsm update` has dropped every entry whose
page the window left. `surface.glsl` holds the call (`NPH_SURFACE_VSM`);
the resolve and the blended surfaces have pipeline variants specialised
on whether the frame has the map, so frames without it keep the registers.

**Overflow and the page budget.** What does not fit the pool,
`max_vsm_meshlets`, `max_vsm_draws`, `max_vsm_voxel_draws` or the walk's
queues (`max_lod_nodes`) is dropped, flagged in
`NphRenderFrameStats.vsm_overflow`, and logged once: pixels whose pages did
not fit the pool are lit, and a page whose draws did not all fit is not read
(it stays dirty, so the lookup passes to a coarser level) and is drawn
again. A frame draws only the pages it wants (new, or dirty and requested:
`vsm_wanted`) that the caster lists hold, a quarter of each kept to spare,
at what a page of each level costs them: the **page budget**, the coarse
levels first (`vsm select`). Every wanted page of the levels from the
coarsest down is granted while they fit, then as many of the next level's
as fit; the others wait for later frames, their pixels reading a coarser
level meanwhile where one holds their point (a dirty page is not read; a
new one has nothing to read). A frame draws at least one page.

The culls count what each level's pages drawn take of each list: the
meshlet items, the caster draws, the voxel casters' draws, and the walk's
nodes (as their share of its fullest queue), counted past a list where it
overflowed (a subgroup's items are mostly of one level, and count
together). A level's cost a page is the most a recent frame's pages of it
cost on average, decaying by an eighth each frame that draws one, so pages
dearer than the last frame's seldom overflow the lists; a level not yet
drawn takes the dearest level's, coarse pages costing the more, and the
first frame, with no cost known, draws at most 256 pages. `vsm_budget`
reports how many of the frame's wanted pages the lists could hold (more
than were wanted where all fit). So a burst of pages (the map dropped, the
resolution bias lowered) is drawn over a few frames within the lists, where
it used to overflow them in every frame until the bias rose; the lists'
capacities bound what a frame of such a burst draws.

The **resolution bias** (each step halves every level's resolution, up to 3)
rises when the pool overflows, and when the pages wanted outrun the budget 32
frames in a row without draining an eighth of them (a burst drains, however
slowly; pages wanted as fast as the budget draws them do not) or 4 frames in
a row that each drop every page (the sun turning every frame: each frame
wants all its pages again). The lists' overflows do
not raise it: the budget shrinks to what the pages cost. 120 frames in a row
whose pages would fit a step finer within three quarters of the pool, within
the budget, and, in frames that drop every page, with four times their pages
within the budget, lower it again (`vsm_lod_bias`).

In the 4K field the map holds 946 pages (62 MB) and adds 0.40 ms to a
still frame: the requests 0.074 ms of the first pyramid's pass, the
lookup 0.27 ms of the resolve, its own passes 0.05 ms. A sphere moving
every frame redraws 20 pages and adds 0.44 ms; the sun's rays add
0.42 ms still and 0.79 ms with motion (ADR-0033).

## The production path

`view.pipeline = NPH_RENDER_PIPELINE_PRODUCTION` draws the frame with the
GPU-driven visibility-buffer renderer (Haar & Aaltonen, "GPU-Driven
Rendering Pipelines", SIGGRAPH 2015; Burns & Hunt, "The Visibility
Buffer", JCGT 2013; Karis et al., "Nanite: A Deep Dive", SIGGRAPH 2021).
The CPU writes the view, the records of instances that changed, and the
pass declarations, a few microseconds whatever the object count
(`render_perf`, docs/benchmarks.md); the GPU culls, rasterises, and
shades. Blended surfaces stay on the forward pass, drawn over the result.
The frame has no depth buffer for them: their fragment stage reads the
opaque depth from the visibility buffer (its sampled R32G32_UINT view,
the depth's bits in y) and demotes a fragment that lies behind it, as the
reference's `GREATER` test would reject it. Demoted fragments still serve
their quad's derivatives. Their page requests (`vsm_request.frag`) and the
depth-tested debug lines test the same way. A pass copying the depth into
a D32 attachment for them (`vis depth`) cost Bistro 0.19 ms at 4K; testing
in the shader costs its blended passes 0.015 ms.

It needs 64-bit image atomics, 64-bit integers in shaders, subgroup ballot,
arithmetic, and shuffle, and the geometry-shader feature (which lets a
fragment shader read `gl_PrimitiveID`). `nph_renderer_production_supported` says
whether the renderer has it; a view asking for it on a device without it
is drawn by the reference renderer, logged once. `disable_production` in
the renderer's description skips it (no GPU memory, no pipelines).

### The GPU scene

The renderer keeps GPU tables indexed by pool slot, so nothing is
re-sent per frame:

| Table | Holds |
|---|---|
| instances | `NphVisInstance`: the transform (camera-relative, below), mesh slot, material range, flags (visible, mirrored, similarity) |
| previous transforms | last frame's transform per slot, for phase 0's occlusion test |
| meshes | `NphVisMesh`: dequantisation, bounding sphere, stream and meshlet addresses |
| material map | per instance a range of material slots, from a TLSF range allocator over offsets (`core/range_alloc.h`) |
| lights | `NphVisLight`: the shading record, its position split like a translation, its range, and whether the slot is live |

Creating, moving, hiding, or destroying an instance marks its slot; each
frame writes at most `max_instance_updates` update records into the
frame's upload buffer, and `vis scatter` copies them into the tables (the
rest wait a frame, logged). A destroyed instance's material slots return
`NPH_GPU_FRAMES_IN_FLIGHT` frames later, after the frames that may read
them; the range allocator has room for a whole population being replaced
between two frames. A light created, changed, or destroyed goes up the
same way in the next production frame; the update list holds every
light, so none waits.

Translations stay 64-bit on the CPU. The GPU gets each as a high and a
low float and the camera likewise, and subtracts them part by part
(Cozzi & Ring, "GPU RTE", 2011), so positions 10,000 km from the origin
render as they do at it (a test).

Meshes are split into meshlets of at most 64 vertices and 124 triangles
when created (or take the cooked ones), each with a bounding sphere and a
normal cone. Without mesh shaders the renderer also widens each mesh's
meshlet triangles into a shared pool of 16-bit indices for indirect draws.

### Levels of detail

A mesh can carry a hierarchy of cluster levels of detail (CLAUDE.md §8.6
stage 9; ADR-0060; `render/cluster_lod.h`), as
`nph_render_cluster_lods_build` makes it from the mesh's meshlets (level 0)
and `NphRenderMeshDesc` takes it. The production path then draws each part
of the mesh at the coarsest level whose error, projected from the part's
distance, is within the view's threshold (`NphRenderView.lod_error`, in
pixels; 1 by default), so that a distant mesh costs a few clusters and a
large one (a terrain) is fine where the camera is and coarse far from it.

**The levels.** Each level is made from the one before, after Batched Multi
Triangulation (Cignoni et al., IEEE Visualization 2005) as Nanite does it
(Karis, Stubbe & Thomas, SIGGRAPH 2021): the level's clusters are grouped
by eight neighbours (grown from Morton-ordered seeds by the vertices they
share); the vertices a group shares with another are locked; the group is
simplified to half its triangles; the result is split into clusters again.
Because a group's boundary does not move, the clusters a group made can
stand in for its own wherever its error is small enough, with no crack
where the levels meet. The simplifier (`cluster_simplify.c`) collapses
half-edges, each moving a vertex onto a neighbour, ranked by quadric error
(Garland & Heckbert, SIGGRAPH 1997): every level indexes the mesh's own
vertices, and no attribute is interpolated. Vertex kinds after
meshoptimizer's simplifier (Kapoulkine 2017) keep attribute seams and
material borders in shape: an interior vertex moves onto any neighbour, one
on an open border or a seam only along it, a corner of seams or a vertex of
a non-manifold edge never; the link condition keeps the topology, and no
collapse may turn a triangle over, fold it to a line, or face it away from
the level-0 normals merged into its corners.

A group's error, in mesh units, is the largest of its collapses' quadric
error, two measured distances, and the errors of the groups that made its
clusters: every level-0 point against the level's whole result (through the
class it merged into; a point by a locked border may lie under the
neighbour's triangles), and the result's triangles' centroids and edge
midpoints against level 0 (a chord across a curve lies farther from it than
any of its points does from the chord). On curved test surfaces the
quadric alone came out 2.5 to 3.3 times under the measured distance; with
both, the points of a coarse level's triangles (corners, edge midpoints,
centroids) lie at most 0.86 to 0.99 of their makers' errors from the true
surface, past level 0's own chords
(`a_groups_error_bounds_how_far_its_clusters_stray`). Errors never fall going
up and each group's sphere holds those of the groups that made its
clusters, so one threshold cuts the levels in one consistent place. The
levels stop at one cluster, or when a level takes off less than a tenth of
the triangles (a flat-shaded mesh, whose every vertex is a seam corner,
stops at once). The coarsest level's clusters are grouped as any level's,
into root groups whose error is the largest float: so a mesh that stops
early with hundreds of coarse clusters (foliage, seams everywhere) has them
culled and handed on a few at a time.

**The build at once.** A level's groups are built at once, as jobs, where
the input gives a job system (`NphRenderClusterLodInput.jobs`; the cooker
gives its own). Groups that share a class are coloured apart (greedily, in
group order); the colours run in turn and a colour's groups at once, each
reading and writing the quadrics, normals, and lists of its own classes
alone, so that a group sees what its neighbours of earlier colours made of
the classes they share, as a group built after its neighbours always has.
A group numbers its vertices and classes with hash maps of its own and
builds its clusters, voxels, sources, and orphans into arrays of its own,
joined to the level's in group order once the level's groups are done. The
measured error then runs a class at a time and a group at a time, each a
function of the level's result alone. So the build is deterministic, the
same to the byte on any number of workers and on none, and takes about
12.4 µs a triangle on one thread and 2.4 on fifteen (`cluster_lod_torus_262k`,
`cluster_lod_torus_262k_jobs`, benchmarks/render; ADR-0060's amendment of
2026-10-27).

**The hierarchy.** Each level's groups, and the root groups, in Morton
order of their centres, lie under trees of nodes of eight children, the
trees under node 0. A child (a node or a group) records its sphere,
the largest group error below it, and the smallest error of the groups that
made the clusters below it (0 where level 0 is). Every sphere is the union
of its parts rounded up in double precision, so it holds them exactly.

**The cut.** A cluster is drawn when the group that made it projects an
error within the threshold and the group that consumes it does not; a
root group (error the largest float) never does. With `k` the pixels a unit
spans at a distance of one over the threshold, a group of error `e` and
sphere (`c`, `r`) under an instance of scale `s` is too coarse when
`s e k > max(|c| - s r, near)`. One function, `vis_lod_group_too_coarse`,
decides it for both the walk and the meshlet cull, `precise` throughout and
without a square root, from numbers the hierarchy must copy exactly from
the group: the two decide alike for every group, bit for bit, or a part
would be drawn at two levels or at none.

**The walk.** `vis cull instances` hands a visible instance of a mesh with
levels to the walk as its hierarchy's root (an (instance, node) work item),
instead of handing on its meshlets, unless even the finest group that
consumes level 0 (`NphVisMesh.lod_level0_error`) is surely too coarse from
the far side of the instance's sphere: then the cut is level 0 whole, and
the instance hands on level 0's meshlets as a mesh without levels does
(near instances, most of Bistro's). Each step of `vis cull nodes`
(`vis_cull_nodes.comp`) takes two of the hierarchy's levels: a work item
takes 64 lanes, one per child and grandchild of its node. A lane takes a
child that is too coarse (the walk's test of a node is loosened past the
rounding between it and the exact test, so it loses no group below), unless
every maker below is too coarse even from the child's far side (then every
cluster below would be replaced in turn), inside the frustum and in phase 1
not behind the depth pyramid. A group it takes appends its clusters to the
phase's meshlet list, where the meshlet cull drops those whose makers are
too coarse and tests the rest as any cluster; a node at the second level
goes into the next queue. The queues are three in turn: a step reads one,
fills the next and raises the next step's indirect arguments itself, and
empties the third for the step after the next, so the steps follow one
another with no pass between. The frame takes as many steps a phase as the
deepest live hierarchy needs, half its node levels rounded up (`vis_lod_steps`
in the frame's statistics). Phase 0 walks the instances it found visible;
phase 1 those phase 0 deferred, against this frame's pyramid.

A threshold of 0 turns the levels off: every mesh draws level 0's
meshlets, in the view and in the shadow map's pages, the same image byte
for byte as the mesh without levels. The sun's shadow map draws cuts of its
own ("The sun's virtual shadow map", above); the reference renderer and the
bottom levels of the shadow rays and the radiance cache draw level 0 for
now.

**Creation.** `nph_render_mesh_create` checks the levels so far as the
GPU's reads stay inside their streams and its walk ends (every index; a tree
from node 0, at most 32 node levels deep), and so far as the walk cannot
lose a cluster it should draw (a group's numbers in its node are its
record's, its maker error no larger than its clusters' makers' smallest,
and a node's bounds hold its children's); `NPH_RESULT_INVALID_ARGUMENT`
otherwise. That the levels cut cleanly is the builder's contract.

**A mesh of voxels alone** (a cell's proxy, "Cell proxies" below) has no
vertices and no indices: its submeshes name materials alone, it has no
meshlet vertices or triangles, and every cluster of every level is a voxel
cluster, level 0's too, its meshlets and levels given (none can be built
without triangles). It gets no vertex or index buffers and no bottom level:
the production path draws it, the reference renderer passes it by (no
draws), and rays and the reference's top level leave it out.

**Cost.** In `lod_4k_production` (render_perf: a terrain 400 m across of
131,072 triangles and 1,024 rocks of 65,024 each, 67 million in all, from
head height) the frame draws 1.15 million triangles in 18,300 clusters and
takes 3.70 ms; at level 0 (`lod_4k_level0`) 4.30 million and 3.99 ms, the
culling and raster passes 0.70 ms against 0.99. The walk visits 4,700 nodes
in three steps a phase, 0.040 ms in phase 0 and 0.029 ms in phase 1; an
empty step costs 2.7 µs. ADR-0060 records how it got there from 0.192 ms.
Bistro cooked with its levels (docs/cook.md, step 8), against itself at
level 0 (`render_perf --lod-error=0`), interleaved: street +0.03 ms, walk
-0.02, turn 0.00, terrace 0.00, hedge -0.02, within the runs' spread of
0.05 to 0.1 ms; the street draws 1.15 million triangles against 1.42, its
walk 0.03 + 0.02 ms. Its geometry is near, where level 0 is the cut.

### Voxels

Aggregate geometry (foliage, twigs, debris: many small disconnected pieces)
stalls the levels' collapses, which cannot take a closed piece away: the
landscape's pine sapling stops at 95,716 triangles in 1,539 clusters, and a
forest of them drawn at that level overflows every list. Given a material
sampler, the builder makes such groups voxels instead (ADR-0061;
`render/voxel.h`, `cluster_voxel.c`), as Nanite's foliage does (Epic,
"Nanite Foliage", UE 5.7). ADR-0061's first four stages are in: the
voxels, their levels, the voxels' tests and the shading model's terms on
the CPU; the mesh format (docs/asset.md) and the cooker's material sampler
(docs/cook.md, `--voxels`); the production path's voxel raster and resolve
(below); and the sun's shadow map's voxel casters ("Voxel casters" under
"The sun's virtual shadow map"). The cooker makes voxels only when asked
until the raster draws the reference scenes' foliage faster than their
stalled triangles ("Measured on the GPU").

**The records.** A voxel cluster is a cluster whose meshlet record has no
vertices, past level 0 (a mesh of voxels alone, below, has them at level 0
too): `triangle_count` bricks from `vertex_offset`, at most 32 bricks
and 1,024 voxels. A brick is 4 x 4 x 4 cells of one power-of-two
size on the mesh's grid, with a 64-bit occupancy mask; a voxel is its cell's
footprint (a box in 32nds of the cell, the ellipsoid inscribed in it, or a
capsule between two of its opposite corners with a radius of `2^(c/8 - 8)`
of the cell), albedo and ambient occlusion, roughness and metallic, and a
two-sided von Mises-Fisher lobe of normals (an axis and a concentration, its
byte `kappa = 2^(b/16) - 1`).

The builders make bricks of 32 bytes and voxels of 16 (render/voxel.h); a
mesh stores and draws them compact (`nph_render_voxels_pack`; ADR-0063 "The
encoding and the minimum"): a voxel cluster's record (8 bytes, which its
meshlet names in `triangle_offset`: its voxels' first word, its cell's
exponent, and the occlusion and metallic its voxels share), a brick of 16
bytes (its corner in whole bricks, exact on its grid, its first voxel's rank
in its cluster, its mask), and a voxel of three words: the footprint's box
and shape, the lobe (its axis octahedral at 12 bits a component, or a
capsule's at 8 beside its byte, then the concentration), and the albedo and
roughness. A cluster whose voxels differ in occlusion or metallic is wide,
its voxels four words with their own. Lossless but for the axis, within 0.06
degrees (a capsule's, whose lobe is broad, 0.95), the form takes 0.72 times
the memory: the landscape's proxies from 2.06 GB to 1.48. The shaders read it
through `vis_voxel_cluster`, `vis_voxel_brick` and `vis_voxel_load`
(vis_voxel.glsl), which give the tests the levels' layout back bit for bit,
twins of `nph_render_brick_from_packed` and `nph_render_voxel_from_packed`.

**When a group becomes voxels.** A group of at least 256 triangles whose
pieces average at most 32 triangles (union-find over its vertices'
positions: an open sheet or a flat-shaded mesh that stalls on its borders or
seams is one piece or a few, and stays triangles), every submesh opaque or
masked, becomes voxels where its simplification keeps more than three
quarters of its triangles, or where its quadric error passes the voxels',
or where voxels at most twice as coarse as the triangles it keeps number
no more than those (a level's step, which voxels then continue at a quarter
a level where aggregate triangles only halve: a pine's needles do not
stall, but their triangles a pixel grow with distance; ADR-0061's amendment
of 2026-10-28). A group holding a voxel cluster always does. The first cells are the
smallest power of two at least twice the error so far whose cells the
triangles overlap at most four times their count (a stalled level's error
is its finest pieces', so halving its primitives would jump the error, and
every distance between would draw the stalled triangles); coarser voxel
groups take at least their coarsest input's cell and twice their finest's,
and at most half their inputs' primitives: a group of one cell doubles it,
and one whose parts became voxels at different levels brings the fine ones
to the coarse ones' cell where that halves them (twice the coarsest cell
raised every part's error to it, and kept the fine parts' finer clusters in
cuts out to that error: a pine's cut at a metre held 163 clusters where it
now holds 38). (A group of a few voxels across the mesh's origin takes as
few as it can have: the grid of every cell size divides each axis at zero, so
it lies in two cells along each axis it crosses however large they grow;
the pine sapling's trunk stands there). A voxel group's error is its cell.
Every voxel level is made again from the triangles where they first became
voxels, each in one cluster of each level (the cluster of the cell holding
its centroid), never from finer voxels, which would lose the correlation the
footprints measure.

**A cell's voxel** (`lod_voxelize`). Each cell the triangles overlap
(Akenine-Möller's triangle-box test) is probed by 16 x 16 jittered rays
along each axis, against its triangles over the cell's own extent (a masked
submesh's points through the sampler's `covers`):

- *Coverage*: the share of an axis's rays that hit, the union of the
  triangles' projections, so that geometry behind other geometry in the
  cell counts once (Heitz & Neyret, HPG 2012).
- *Footprint*: within the box of every hit point, widened by half a ray's
  spacing.
  - A *surface's* (its normals' second moment 0.9 or more along one axis,
    and the box's shadow along the axis of most coverage within a quarter of
    that coverage: a sheet across the cell fills it, a slanted sliver does
    not) is that box, which closes up with its neighbours'.
  - A *needle's* (its hits strung along one line: their spread's largest
    eigenvalue 0.9 of the whole) is a capsule along that line between the
    hits' farthest points on it, of the radius of their spread across it
    (the hits lie on the needle's surface; a needle a hundredth of the
    face across meets only two or three of an axis's rays, so its coverage
    is known only to about 40 %, its surface points exactly), held to the
    cell. Its projected area follows the needle's as the view turns, down
    to its section end-on, as no axis-aligned shape's can.
  - An *aggregate's* is sized so its projected area along each axis is the
    coverage: the ellipsoid inscribed in a box, whose projected area along
    a direction `d` is `sqrt((c_x d_x)^2 + (c_y d_y)^2 + (c_z d_z)^2)` of
    the cell's face, as an SGGX distribution's is (Heitz et al., SIGGRAPH
    2015), where a box's is `c_x |d_x| + c_y |d_y| + c_z |d_z|`, up to
    `sqrt(3)` times an isotropic cloud's along a diagonal; a box where an
    ellipsoid cannot reach the coverages. It lies at a place within the
    hits' box hashed from the cell: footprints that sat in their cells'
    middles would line up along each axis and let rays down a column
    through, where the pieces are independent from cell to cell (Cook et
    al., SIGGRAPH 2007).
- *Look*: albedo, roughness, metallic and occlusion averaged over the
  rays' first and last hits (what is seen from outside the cell), every
  other ray each way; the lobe from those hits' normals, each weighed by
  `1 / (|n_x| + |n_y| + |n_z|)` so that the three axes' views count the
  surface by its area: the axis the second moment's dominant eigenvector,
  the concentration the one whose second moment along its axis is the
  dominant eigenvalue.

**A voxel's test** (the shaders' `vis_voxel_hit` in `vis_voxel.glsl`, and
its C twin in `voxel.c`) takes the ray's span through the voxel's cell from
the cell's own planes, then tests the footprint over that span. A voxel is
tested apart from its neighbours, so every pass that tests it meets it at
the same `t`, and the cells' and footprints' planes are exact (cells are
powers of two, their corners small multiples of them). The round shapes'
quadratics are taken from where the ray enters the cell, so their terms
keep their precision however far the ray came, and a capsule's is the
earliest entry of its body and its ends' spheres within the cell. A ray's
first voxel in a brick (`nph_render_voxel_trace`) is the nearest of its
voxels' hits, the lowest cell on a tie. Against a search of every footprint
in double precision (the capsule as the points within its radius of its
segment, by bisection), 25,600 rays at 400 random bricks of every shape
agree on every hit (3,965), and on its `t` within 2e-5 of the brick. The
shaders' twin, every value `precise` so that no shader rounds it
differently from another, agrees with the C one on 16,384 rays through 256
random bricks (2,609 hits): every hit, voxel and face, `t` within 6e-6 of
the brick, on the RTX 3060 Ti and on lavapipe alike; and the resolve's
search from each hit's depth (below) meets that hit again, bit for bit.

**The shading model** (`nph_render_voxel_diffuse` and its fellows). For
the lobe turned toward the viewer, the diffuse response to a light is the
clamped cosine toward it averaged over the normals the viewer sees, `D(m)
(m.v)+` normalised. The viewer's cosine is taken as a spherical Gaussian of
sharpness 2.8, whose product with the lobe is a lobe again (Wang et al.,
SIGGRAPH Asia 2009), and that lobe's mean clamped cosine toward the light
has Stephen Hill's fitted closed form (in Pettineo, "SG Series Part 3",
2016), which is the cosine itself for a lobe of one normal: a sheet's voxel
shades as its triangles. Against quadrature of the definition, over a grid
of lobes, views and lights, its error averages 0.7 % of a facing light's
response, at most 5 % within 60 degrees of the lobe's axis, and 8.3 % at a
grazing 84 degrees, where the Gaussian does not cut the lobe at its horizon
as the cosine does (a viewer's sharpness of 2.8 measured best of those
tried; one rising with the grazing angle traded the error between lobes).
Specular is GGX with the lobe's variance added (`alpha^2 + 2 / kappa`), and
the environment's harmonics are convolved with the lobe (bands 1 and 2
scaled by `A(kappa)` and `1 - 3 A(kappa) / kappa`).

**In the production path.** The meshlet cull sends a voxel cluster to a
raster list of its own (`NPH_VIS_RASTER_VOXEL`, flagged
`NPH_VIS_CLUSTER_VOXELS`), whatever its material: a voxel is opaque and
carries the look its mesh's own materials had. `vis voxel raster`
(`vis_voxel_raster.comp`) takes a workgroup a cluster and splats its voxels,
an invocation a voxel. Each voxel projects its footprint's box (a capsule's
widened by its radius, within its cell) to the pixels whose centres it may
cover. The box's corners lie on its brick's grid of 32nds of a cell, so its
centre and half-extents go to clip space as steps along that grid, the same
for the whole cluster (a cluster's bricks share a cell size). Its projection
lies within `(r_x + |x_c / w_c| r_w) / (w_c - r_w)` of its centre's in x,
and likewise in y, where `r_x` and `r_w` are the half-extents' reach in clip
x and w, widened by a 64th of a pixel against rounding. A subgroup pools its
voxels' pixels (a prefix sum, then a binary search for each pair's voxel),
so that the rays' tests fill its lanes whichever voxels cover pixels: at the
first voxel levels a cell is about a pixel and a foliage footprint a
fraction of its cell, so about one voxel in five covers a pixel's centre.
Each pair's ray, from the camera through the pixel's point on the near
plane, goes into the instance's space and is tested against that voxel
alone; a hit takes the atomic max of (depth, cluster, brick) as triangles
do, so the nearest voxel wins. Under reverse-Z with an infinite far plane a
hit at `t` along that ray lies at depth `1 / t`. `vis resolve` writes
nothing at a voxel cluster's pixel: the first invocation of a subgroup that
saw one sets its tile's bit (`NphVisViewData.voxel_tile_bits`, cleared by
`vis clear`), and the one that set it lists the tile and raises `vis voxel
resolve`'s workgroups. That pass (`vis_voxel_resolve.glsl`, compiled and
specialised as the resolve is) finds each of those tiles' voxel pixels'
voxel again in the brick the visibility buffer names (`vis_voxel_find`): it
tests the cell holding the ray's point at the stored depth first, then the
two cells along each axis whose centres lie nearest that point (which hold
it whatever its rounding), and stops at the first whose hit gives the
stored depth again, bit for bit the raster's. That gives the voxel, its
point, and the face of its footprint the ray entered (the geometric
normal), and the pass shades it with `surface.glsl`'s
`nph_surface_shade_voxel`: the lights and the light grid, the sun's map,
shadow rays, the ambient occlusion and the radiance cache as the triangles
have them, with the lobe's terms above.
Its point's shadow lookups and rays, and its cell of the radiance cache,
start a cell's diagonal off that face, clear of the triangles the voxel
stands for, which the shadow map and the rays still draw; `gi update` takes
a voxel pixel's point and face the same way. The frame's statistics count
the voxel clusters drawn and their bricks (`vis_voxel_clusters`,
`vis_voxel_bricks`), and every debug view shows voxels as it shows triangles
(the triangle view names the brick).

**Measured** (`test_voxel.c`): a sheet across cells and a slanted one leave
no ray through them unstopped (4,000 and 4,500 rays); a cloud of 2,000
needles keeps its coverage within 9 % from the axes, the diagonals and a
random direction (0.069 against the needles' 0.073 along x, 0.082 against
0.075 along the cube's diagonal); a needle across a cell becomes a capsule
whose coverage is within 11 % of the needle's from six directions, and a
dot end-on; cuts through a foliage mesh's levels at 300 m and 1.2 km are
all voxels and cover what level 0 covers (1,778 rays of 9,216 against
1,777, with 174 and 4 clusters); a wavy sheet's levels make no voxels.
ADR-0061 records the footprints these replaced and what each measured. The
build of 2,000 closed
needles (16,000 triangles) takes 0.89 s on one thread, about 55 µs a
triangle, most of it in the cells' rays.

**Measured on the GPU** (`test_vis_voxel.c`, the RTX 3060 Ti and lavapipe
alike): a foliage mesh of 1,500 six-sided needles draws its voxel clusters
exactly where the CPU's rays through the clusters drawn meet them, every
pixel of the cluster debug view (0 of 476 and 105 pixels differ at 12 and 24
m; 7 of 1,298 at 7 m, where triangles mix in, at the hardware raster's
edges); the voxel resolve shows the voxels' albedo on every voxel pixel, and
its lit pixels are its C twin's within 0.1 % on average and 2.3 % at worst
(the HDR target's dithered rounding). Against level 0, over a grid of
sixteen instances turned every way: the cut covers 3.7 to 7.1 % more pixels
at the first voxel levels and 4.9 % fewer at the coarsest, and reflects 7 to
10 % more light a pixel; ADR-0061 "Accuracy" records why and the work that
will close it. The radiance cache's update keys the voxel pixels' cells
where the CPU's rays through the clusters drawn put them (284 cells from 307
voxel pixels at 30 m, every one of them and nothing else).
`render_perf`'s `foliage_4k_production` (1,024 shrubs of 4,000 sparse
needles) draws about 3.2 million bricks of 24 million voxels a frame in 7.6
ms, its voxel raster 3.4 ms and the voxel resolve 0.069 ms, against 5.6 ms
for the shrubs' stalled triangle levels (`foliage_4k_triangles`). A voxel
costs the raster about what a triangle does (some 140 ps against 146), but
the first voxel levels hold about twice the triangles they replace, and
four in five of their voxels cover no pixel's centre: on sparse foliage the
voxels still cost more than the stalled triangles, which is why the cooker
makes voxels only when asked for now. Splatting took the voxel raster from
5.5 ms (each pixel of a brick's rectangle walking its cells: 15 million rays
taking 60 million steps for 1.8 million hits) to 3.4 ms. The first voxel
level's budget of four times its triangles stays: at one, the simplified
triangle levels drawn in its place from 24 to 40 m cover 11.6 to 12.5 %
fewer pixels than level 0 (needles thinned below a pixel), where the
voxels cover 3.7 to 7.1 % more (ADR-0061 "Cost"). A frame costs the voxels
nothing until the renderer holds a mesh with voxel levels: the meshlet
cull, the resolve and the radiance cache's update are specialised without
the voxels' routing, test and search (`k_voxels`), and the voxel passes and
their tile list are left out. `render_perf`'s 4K field, LOD and shadow
scenes and Bistro's street measure within 0.004 ms of their frames before
the voxels, the resolve unchanged; compiled in, its test alone, never
taken, had cost it 0.5 to 1.7 %, wherever in it the test stood.

### Cell proxies

A world partition's cell is drawn from far off as one mesh of voxels, its
proxy, in place of its instances (ADR-0062; `render/cell_proxy.h`,
`cell_proxy.c`). Stage 1 made the builder, on the CPU, with its tests; the
formats (docs/asset.md: a mesh of voxels alone, a scene's cells); and the
cooker's cells (`nph_cook --hlod`, docs/cook.md "Cells"). Stage 2 makes the
renderer's cells and their switch ("The switch", below).

**What a proxy is.** A mesh of voxel levels alone (ADR-0061's records): its
level 0 at the proxy's finest cell (0.25 m for the landscape's 560 m), each
next level twice as coarse, to a level of at most two cluster blocks along
each axis. Each level's clusters hold a block of 8 x 8 x 8 cells (2 x 2 x 2
bricks: at most 512 voxels), so the levels are an octree: a group consumes
the clusters of a block of 16 cells of its level and makes the one cluster of
the next level there, its error that cluster's cell, its sphere round its
makers' spheres and the cluster it makes (a coarser cell's footprints may
reach past the finer ones'); level 0 is made by no group, and one root group
takes the last level. The records are `render/cluster_lod.h`'s, the hierarchy
built as a mesh's is, so the renderer walks, culls, draws and shadows a
proxy as any mesh with voxel levels.

**Sampling.** Each level is voxelized as a mesh's voxel levels are, a
jittered grid of 16 x 16 rays along each axis of each cell, from each
instance's own cut at half the level's cell through its transform: its
triangles, their corners in the proxy's space and their points sampled by
their mesh's materials (the normal taken across by the inverse transpose),
and its voxels as footprints, each ray taken into the voxel's mesh space and
tested as the renderer tests it, forwards and back for where it enters and
leaves (`voxel_footprint_span`; a span of no length is a touch from the next
cell, not a hit). A footprint's look is its voxel's and its normals' second
moment its lobe's, `((1 - b2) / 3) I + b2 n n^T`. A level is voxelized in
tiles of 64 of its cells along x and z, the sources gathered for each, which
bounds the memory to a tile's.

**Visibility.** What no camera at the proxy's distance or more sees is left
out (Gobbetti & Marton's environmental occlusion, "Far Voxels", 2005): the
cell's voxels at the visibility level (twice the finest cell by default) and
the occluders' around it (the neighbouring cells' instances and the terrain,
within given bounds, at twice that cell) are swept from 96 directions over
the sphere (a Fibonacci lattice), each with a grid of parallel rays half a
cell apart, the voxels front to back by their cells' centres; each ray stops
at the first footprint it meets, which is seen, and a footprint smaller than
the rays' spacing that no ray meets is seen where a ray through its cell is
still open. Rays, not opacities: a wall's voxels close up, where a per-voxel
opacity would let half the light through one at a slant. The seen cells'
neighbours are kept too, against a gap the directions missed. The finer
levels keep the cells inside seen ones, the coarser the cells holding one.

**Measured** (`test_cell_proxy.c`). A bush inside a closed double shell (an
outer and an inner box 0.55 to 0.7 m apart): none of its 403 level-0 voxels
is seen, 1,783 of the 2,907 voxels are, and the neighbours bring back 889.
One closed surface alone does not hide: where its faces meet in a cell, the
cell's footprint is fitted to their coverage and may fall a 32nd short of the
cell's face, a slit the renderer would see through too. Inside an occluders'
shell nothing is seen, and the proxy is empty. A turned sheet's 81 cells are
all sheets facing its turned normal, in its material's colour. Four turned
trees of sparse needles, over 32 seeds of their needles: the proxy's finest
level stops 4 to 7 % more rays on average than the trees' own cut at its cell
along the axes, and 11 to 12 % more along slants 10 degrees below the horizon
between them (a seed's direction from 2 % less to 26 % more); that cut itself
stops 0 to 3 % more than the needles. Voxels made from voxels stop 3 % more
rays than their sources where their grids agree (6 % along the axes; 2 %
through a quarter turn), and more refitted in a turned grid (ADR-0062 "Stage
1, measured"). The coverage tests' bounds are four standard deviations of
such a sweep's spread, per direction and for the mean over the directions:
one seed is one realization of the needles and of the voxelizer's jittered
rays. The voxelizer's tests take each footprint shape back as a source
within the 32nds its ends round to, keep the cells a filter names and those
alone, and see a voxel and triangles in one cell as the triangles of its box
are seen.

**The switch** (stage 2). `nph_render_cell_create` makes a cell: the box of
its instances (a 64-bit centre and half extents), its proxy (a mesh of voxels
alone, its materials, and a rotation and translation), the proxy's finest
error (its level 0's cell) and its distance `D`. The cell owns the proxy's
instance; an instance names its cell in its description, and the cell's
index, plus one, rides in the upper 24 bits of its record's flags
(`NPH_VIS_INSTANCE_CELL_SHIFT`), the proxy's with `NPH_VIS_INSTANCE_PROXY`.
The cells' records (`NphVisCell`: the centre split into high and low floats
as a translation is, the half extents, the error, and `D`) sit in a table the
instance culls read.

- *The camera's.* At the start of the instance cull, a cell's instances and
  its proxy each ask `vis_cell_proxied`: with the levels of detail, is the
  box's nearest point at least `D` away (what no camera that far sees is not
  in the proxy), and is the proxy's finest error fine from there (the group
  test of "Levels of detail" at that point, scale 1: `e k <= max(d,
  near)`)? Both read the same record and view and do the same arithmetic,
  so the proxy is drawn exactly where its instances are not, and vice versa;
  there is no pass of its own. A view without the levels (threshold 0)
  draws the instances.
- *The sun's shadow map.* Its instance cull asks, per level,
  `vsm_cell_proxied`: is the proxy's finest error within the level's
  threshold (`vsm_lod_threshold` at scale 1)? Then the proxy is drawn into
  the level's pages, the instances into the others. The answer is the same
  every frame, so a page drawn stays right; the sun is infinitely far, and
  the proxy holds what is seen from there.
- *Rays* (the shadow rays, the radiance cache) trace the instances' bottom
  levels: a proxy has none, and the top levels leave it out.
- *The reference renderer* draws the instances; a proxy has no triangles
  for it.

Statistics: `vis_cells_proxied`, the proxies phase 0 kept for their distance
(in view or not), and `vis_instances_proxied`, the instances they stood in
for. The cooked-scene bridge (`samples/common/cooked_scene.c`) makes a render
cell for each of a scene's cells with a proxy, before the instances, which
name theirs; `render_perf --no-cells` loads a scene without them. The
runtime's scene import does not make cells yet: its instances are drawn as
themselves.

**Measured** on the landscape (ADR-0062 "Stage 2, the switch"; its six views
at native 4K, the lists at 16 times): 117 to 208 cells drawn as their proxies
for 13,600 to 26,300 instances, and the frame 0.4 to 4.2 ms faster (the
valley 57.3 to 53.1 ms): the voxel raster saves 2.2 to 3.2 ms, the walk 0.4
to 0.6, and the instance cull takes 0.5 ms more, handing proxies their finest
level whole between their distance and about twice it. The image changes at
the far ridges' silhouettes alone (FLIP 0.014 to 0.029 over the frame,
against 0.0001 to 0.0035 between two runs without cells). Held whole, the
landscape's proxies (2.06 GB decompressed) take the GPU's device-local memory
from 3.96 GB to 5.84 GB with the default lists, and the host holds them twice
over (the cooked scene keeps its assets loaded: decompressed, and mapped where
they were read): they are to be streamed (stage 3).

### A frame

| Pass | What it does |
|---|---|
| `vis clear` | clears the counters (and on the vertex path the visibility buffer, which the mesh path's raster clears as its attachment) |
| `vis scatter` | applies the frame's instance updates |
| `vis light updates`, `vis lights`, `vis light bins` | the light grid's lights and distance bins (below), from the light table; nothing here reads the visibility buffer |
| `vis cull instances 0` | per instance: frustum, then occlusion against **last** frame's depth pyramid with last frame's transform and camera; survivors' meshlets go out as (instance, meshlet) work items (a mesh with levels of detail hands its hierarchy's root to the walk instead), the occluded instances wait for phase 1 |
| `vis cull nodes 0` | the walk down the hierarchies of levels of detail ("Levels of detail"), a step per two of their node levels: the groups too coarse to draw hand on their clusters as work items |
| `vis cull meshlets 0` | per work item: frustum, normal cone (similarity transforms only), occlusion; survivors are appended to the cluster list and to a raster list: opaque, masked, software, or voxel |
| `vis raster 0`, `vis raster 0 masked`, `vis sw raster 0`, `vis voxel raster 0` | the hardware raster of the opaque clusters (on the mesh path through the ROP), then of the masked ones (their skin first, below), then the compute raster of the small ones, then of the voxel clusters ("Voxels"; only while the renderer holds voxel meshes), into the visibility buffer |
| `vsm clear`, `vsm invalidate` | while the sun has its virtual shadow map ("Shadows"): its counters and bitmasks cleared, and the pages this frame's instance updates touch marked dirty, before the scatter replaces their records |
| `vis hzb 0` | the depth pyramid of what phase 0 drew (and the sun's map's page requests, the ambient occlusion's depths, and the light tiles' depth bounds) |
| `vis cull instances 1`, `vis cull nodes 1`, `vis cull meshlets 1` | the deferred instances against the new pyramid (and the walk down the hierarchies of those now visible); then the meshlets phase 0 deferred and those of the instances now visible |
| `vis raster 1`, `vis sw raster 1`, `vis voxel raster 1`, `vis hzb 1` | what phase 1 found visible; the pyramid the next frame tests against, rebuilt only where phase 1 drew (and the map's requests, the occlusion's depths and the light tiles' bounds there) |
| `vis light tiles` | the light grid's tile masks, culled against the tiles' depth bounds (below) |
| `vsm requests (blended)` | with the map and blended surfaces: the blended surfaces' page requests, against the visibility buffer's depth |
| `vsm update` to `vsm statistics` | the sun's map: page tables, allocation, the casters of the pages to draw, and those pages drawn ("Shadows") |
| `ao depth`, `ao`, `ao denoise` | on a view with ambient occlusion: the depths' coarser mips, each pixel's slice, and the denoise (below) |
| `rt instances`, `rt accel` | while a light casts shadows that the sun's map does not take, or the view has global illumination: the top level's records from the GPU scene and its build, when it must be rebuilt, and new meshes' bottom levels ("Shadows") |
| `gi clear`, `gi update`, `gi resolve` | on a view with global illumination: the radiance cache's slots zeroed (its first frame), its rays, and its cells' blend (below) |
| `vis resolve` | shades every pixel from its cluster and triangle (the sun's map read once per pixel; shadow rays for the other lights that cast shadows; the pixel's ambient occlusion; the radiance cache's cell looked up), and lists the tiles that hold voxel pixels |
| `vis voxel resolve` | the voxel pixels of the tiles listed, a workgroup a tile through an indirect dispatch ("Voxels"; only while the renderer holds voxel meshes) |
| `vis statistics` | copies the counters for the frame's statistics |
| `transparent` | the blended surfaces, forward, with no depth attachment: each fragment tests the visibility buffer's depth itself and is demoted behind it (`forward_frag.glsl` `k_vis_depth`) |
| `dof setup`, `dof tiles`, `dof gather`, `dof resolve` | on a view with an aperture: depth of field, as the reference's ("Depth of field") |
| `bloom …`, `exposure …` | with bloom or an automatic exposure: the chain and the metering over the HDR image, as the reference's ("Bloom and exposure") |
| `taa` | on a view with temporal anti-aliasing: the temporal resolve, which mixes in the bloom and tonemaps too (below), or on a frame with motion blur writes only its history |
| `motion`, `motion tiles`, `motion blur` | on a frame with motion blur: every pixel's motion, the tiles' dominant blurs, and the blur fused with the tonemap (below) |
| `debug lines` | after either, the debug lines |
| `tonemap` | otherwise, as the reference's |

`vis args …` passes between them turn counts into indirect arguments,
and appends to the GPU lists are aggregated per subgroup (one atomic per
subgroup). The instance cull writes its meshlets as one work item each,
the subgroup's instances together: one atomic for the subgroup's total,
then every invocation writes an equal share, finding each item's instance
by a binary search over the subgroup's prefix sums, so the meshlet cull
keeps every lane busy whatever the meshes' sizes. This is **two-phase occlusion culling** (Haar & Aaltonen
2015; Karis et al. 2021): phase 0 draws what was visible last frame,
which is nearly everything visible now, and phase 1 catches what
disocclusion revealed, so no object is lost to a stale pyramid. Culling
never changes the image: a test renders the same scene with occlusion and
cone culling on and off and compares.

**The visibility buffer** is 64 bits per pixel (R64_UINT): the depth's
bits high, then the cluster index (25 bits) and the triangle (7 bits). The
raster writes it with `imageAtomicMax`, which keeps the nearest surface
under reverse-Z without any depth test, so the hardware and software
rasters write the same buffer the same way (the mesh path's depth
attachment, below, only spares fragments that would lose the atomic).
The one exception is phase 0's opaque clusters on the mesh path, written
through the ROP (below).

**Raster.** With mesh shaders each workgroup of 32 threads emits one
cluster, culling back faces and triangles that cover no pixel centre
(Wihlidal, "Optimizing the Graphics Pipeline with Compute", GDC 2016)
before output; `gl_PrimitiveID` carries the cluster and triangle. Without
them, `vkCmdDrawIndexedIndirectCount` draws each cluster's slice of the
index pool, and the instance index finds the cluster. Lavapipe has mesh
shaders but loses push constants in their fragment stage
(`NPH_GPU_QUIRK_MESH_FRAGMENT_PUSH_CONSTANTS`, docs/gpu.md), so it takes
the indirect path; the RTX runner covers the mesh-shader path. Masked
materials have their own pipeline, which samples the alpha.

**Early depth.** The mesh path also gives the hardware raster a D32 depth
attachment (transient, cleared to 0 by phase 0 and kept for phase 1,
`GREATER_OR_EQUAL`), tested before the fragment stage
(`early_fragment_tests`), so that a fragment behind what the pass already
drew skips its shader and its atomic. Equal depths pass and the atomic
still decides, so the image is the one the atomic alone makes. An early
test needs a fragment stage that never discards, which is why the mesh
stage culls every back face of a single-sided material, including the
triangles that reach behind the camera: their facing comes from the
homogeneous determinant of the clip-space x, y and w, whose sign holds
whatever the w's (Olano & Greer, "Triangle Scan Conversion using 2D
Homogeneous Coordinates", HWWS 1997). Only the small-primitive box test
needs every vertex in front of the camera. The opaque pipeline writes the
depth. The masked one discards after its alpha test, so it tests early but
writes nothing: an early write would let a discarded fragment occlude. The
software raster's clusters never reach the attachment, so fragments behind
them only lose their atomic, as before. On the RTX 3060 Ti this took
Bistro's `vis raster 0` from 1.135 to 0.735 ms at 4K (0.798 with the
opaque pipeline alone), and the synthetic scenes' by 14–18 %. The vertex
path has no attachment, and discards back faces in its fragment stage.

**Through the ROP.** With that test writing depth, the last opaque fragment
of a pixel to pass it is its nearest, so phase 0's opaque clusters need no
atomic: on the mesh path they draw in a pass of their own whose colour
attachment is the visibility buffer viewed as R32G32_UINT (the triangle and
cluster in x, the depth's bits in y: the 64-bit word's halves), which the
ROP writes in primitive order (`vis_raster_mesh_opaque_rop.frag`), and the
pass clears it as it begins, so `vis clear` no longer does. The masked
clusters follow in a pass of their own with their atomics, tested against
the same depth attachment, then the software raster with its atomics, and
the atomic max merges each with what the ROP wrote. Phase 1 keeps the
atomics for every cluster: its depth attachment lacks the masked and
software surfaces of phase 0, which a write there could cover. Only equal
depths of two triangles part from the atomic's choice (the ROP keeps the
last drawn, the atomic the larger index), both arbitrary, the clusters'
indices coming from atomics in the cull: Bistro's street showed 37 pixels
of instance that differed against 22 between two runs of either. The
clear went (0.085 ms, 67 MB) and the opaque clusters drew faster: Bistro's
street frame took 7.38 ms against 7.50, the terrace 8.19 against 8.30, the
hedge 7.88 against 7.98, the 4K field 4.12 against 4.22 (interleaved,
three rounds). Drawing them with no write at all would have taken 0.16
to 0.24 ms off the pass, so most of the atomics' cost is gone.

**Masked clusters** are where foliage costs: Bistro's views 50° and 90°
from the scene camera (`terrace` and `hedge`, docs/benchmarks.md) look at
alpha-tested cypresses and hedges, and their `vis raster 0` took 1.75 and
2.17 ms against the street's 0.74. Measured on the hedge view, drawing the
masked clusters as opaque (no alpha test, depth written early) would take
1.01 ms and leaving them out 0.43, so most of the cost is fragments of
layer after layer of leaves, each running the alpha test and its atomic.
The mesh stage therefore reads the masked material's alpha test (its
base-colour texture and sampler, alpha factor and cutoff) once a cluster
and hands it to the fragments flat: a masked fragment's only read before
its atomic is the texture, where it was the cluster's record, then the
material's, then the texture. The hedge view rasterises in 1.85 ms and
the terrace in 1.54 (the street unchanged), the same arithmetic in the
same order. Measured and not kept: a masked fragment first reading the
visibility buffer and discarding, before its texture, where a nearer
masked surface was already drawn (the buffer only grows, so the test is
exact): its read cost more than the layers it skipped (+0.03 to +0.06 ms);
and masked depth writes with the test moved after the alpha test, so leaves
would hide leaves: 2.60 ms, since the late test no longer rejects the
fragments behind opaque surfaces before their shader.

**The skin.** Drawing a thin front layer of the foliage that way, before
the rest, keeps both: on the mesh path the cull marks a masked cluster as
its instance's skin where its centre lies within the nearest
`NphRendererDesc.masked_skin` (0.35) of the chord the view ray through it
cuts through the instance's box (the mesh's dequantisation box, in the
instance's own space: a few dozen operations a masked cluster), and lists
it apart (`NPH_VIS_RASTER_SKIN`). The masked pass draws the skin first with
the alpha test and no early test, writing the depth of the fragments that
pass it, then the other masked clusters as before: their fragments behind a
leaf the skin drew are rejected before their shader. The skin's fragments
behind opaque surfaces run their shader, which is the trade the share
sets. The image does not change: the depth written is that of a fragment
that passed its alpha test, which no farther fragment could beat in the
atomic, and equal depths still pass and leave the atomic to decide (a
test renders checkered spheres seen through their holes with and without
it, pixel for pixel). Interleaved on Bistro (two rounds; ms, frame and
masked pass): the hedge 7.85 and 1.46 without, 7.38 and 0.97 with 0.35;
the terrace 8.20 and 1.01, 7.84 and 0.66; the turn 7.84 and 0.93, 7.53
and 0.64; the street unchanged. Shares of 0.25 and 0.30 left more leaves
behind (the hedge 7.56, 7.41 ms), 0.45 to 0.6 drew too many late (7.47 to
7.84 ms). `NPH_RENDER_VIS_NO_SKIN` turns it off; the vertex path has no
depth attachment and draws every masked cluster alike. Measured and not
kept (2026-10-21): the masked clusters drawn through the raster
operations as phase 0's opaque ones are, writing depth after the alpha
test and no atomics: the hedge's masked pass took 1.12 ms against 0.96. A
fragment stage that discards and writes depth loses the early test on
this GPU, even with no other side effects.

**The software raster** (`vis_sw_raster.comp`; Karis et al. 2021) takes
the opaque clusters whose bounding sphere projects in front of the near
plane into at most 64 pixels across and whose triangles average at most
`sw_raster_pixels` (3) across. A 32-thread workgroup per cluster snaps
the vertices to 1/256 pixel, as the hardware does, culls back faces, and
walks each triangle's pixel centres with exact 32-bit edge functions
relative to the triangle's box (the 64-pixel bound keeps them within 30
bits) under the top-left rule, writing the same atomic max as the
hardware path's fragment shader. On the RTX 3060 Ti it gives the hardware
raster's image to the bit. It is faster on micro-triangles (102,400
cubes 1.6 pixels across: 0.38 ms against 0.43) and slower on clusters of a
few larger triangles, which the triangle-size test keeps in hardware.
`NPH_RENDER_VIS_NO_SW_RASTER` (the `r.sw_raster` cvar) turns it off ([ADR-0030](adr/0030-hybrid-raster-and-meshlet-work-lists.md)).

**The depth pyramid** holds the farthest depth (the smallest, reverse-Z)
under each texel. Mip 0 is half the target rounded up, each later mip
halved and rounded down as Vulkan sizes mips; where a source has an odd
number of texels, the destination's last texel takes the extra one, so
mip-m texel t covers mip-0 texels [t·2^m, (t + 1)·2^m) and the last one
reaches the edge. `vis_hzb.comp` writes mip 0 from the visibility buffer
(with the shadow map's page requests and the occlusion's depths, from the
same reads); `vis_hzb_mips.comp` builds the rest six mips a dispatch, after
the single-pass downsampler of Kramer ("FidelityFX Single Pass
Downsampler", AMD GPUOpen, 2020): a workgroup per 64 × 64 tile of the mip
before builds the tile's next six mips in shared memory, the tiling rounded
down so that the last tile of a row or column takes the texels past it and
every footprint lies in one tile. At 4K that is two dispatches where a
dispatch and a barrier a mip took eleven; after phase 1, a tile phase 1 did
not draw into keeps phase 0's mips. Bistro's second pyramid went from 0.134
to 0.109 ms; the first, bound by its full read of the visibility buffer
and its page requests (0.11 ms of its 0.31), stayed as it was. An
occlusion test projects the bounding sphere to a
screen rectangle and nearest depth (Mara & McGuire, "2D Polyhedral Bounds
of a Clipped, Perspective-Projected 3D Sphere", JCGT 2013), picks the mip
at which the rectangle spans one texel, and reads a 2 × 2 footprint. The
frustum, cone, projection, facing, small-triangle, and pyramid tests have
C twins in `reference.h`; a test runs the shaders on thousands of random
cases and compares, and another reads the built pyramid back and checks
every texel is the minimum of what it covers.

Each full build reads the whole visibility buffer (66 MB at 4K, 0.145 ms:
the bus's bandwidth). The pyramid after phase 1 differs from the one
after phase 0 only where phase 1 drew, so phase 1's meshlet cull marks
the 32-pixel tiles each of its clusters' spheres projects onto, and that
pyramid rebuilds mip 0 in the marked tiles alone before the coarser mips
(0.066 ms in the 4K field). A row of tiles is a run of the mask's bits,
set a word at a time, so a cluster of any size marks its rectangle in a
few atomics; only a sphere that does not project (it reaches the near
plane) asks for a full rebuild. Clusters covering more than 64 tiles used
to ask for one too: in Bistro's street a large cluster that phase 0 defers
on every fourth frame (as the jitter moves the edge of what hides it) made
that frame's second pyramid 0.32 ms instead of 0.04, the frame 0.28 ms
longer, and the 1 % low 0.2 to 0.4 ms worse. A test compares the result
with a full rebuild's, texel for texel, after a wall moves off what it
hid. Measured and not kept: reading each 2 × 2 block's depths with one
gather of the visibility buffer's sampled R32G32 view instead of four
64-bit storage loads (what took the temporal resolve's depth search from
0.16 to 0.09 ms): Bistro's first pyramid 0.320 ms against 0.315, the 4K
field's 0.154 against 0.159. Bistro's pass was then the shadow map's
page requests (about 0.14 ms) as much as the read; the block requests
(Shadows) have since cut them to about 0.03 ms.

**The resolve** decodes each pixel's cluster and triangle, fetches the
three vertices, and rebuilds the attributes from perspective-correct
barycentrics. Texture filtering needs derivatives a compute shader does
not have, so it also computes the barycentrics one pixel right and one
down, as the rasteriser's quad would (Schied & Dachsbacher, "Deferred
Attribute Interpolation for Memory-Efficient Deferred Shading", HPG
2015). Facing comes from the sign of the homogeneous determinant of the
clip-space vertices (Olano & Greer, "Triangle Scan Conversion using 2D
Homogeneous Coordinates", 1997), right even with a vertex behind the
camera. Shading is `surface.glsl`, as above; the images match the
reference's (100 % of pixels within 3 levels in the tests). One pass
covers every pixel: classifying the tiles of background alone into a
pipeline of their own measured slower (ADR-0031). So did running its
workgroups in vertical strips of 64 for the L2's sake (Bavoil, "Optimizing
Compute Shaders for L2 Locality using Thread-Group ID Swizzling", NVIDIA
DevBlog 2020), in runs interleaved with the dispatch's own rows: remapped
by divisions, the 4K field's resolve took 1.29 ms against 1.21 and
Bistro's 3.64 against 3.59; dispatched as (strip, rows, strips), 1.24 and
3.60.

Where Bistro's 3.6 ms goes, from builds with one part compiled out
(2026-10-20): reading the visibility buffer and storing the colour 0.19
ms; rebuilding the attributes (the cluster, instance, mesh and meshlet
records, the triangle's corners and vertex indices, then the vertices:
eight dependent loads) about 1.0 ms; the local lights 1.02; the material's
textures 0.93; the sun's virtual shadow map 0.34; the radiance cache 0.21;
the occlusion 0.15; the environment 0.08 (the parts overlap, so they sum
to more). The pass is bound by the latency of those chains: with fewer
warps resident it slows in proportion (28 warps an SM: 3.60 ms; about 20:
4.72; about 12: 6.96). The variant with both the shadow map and the cache
held 72 registers and spilled 2,304 bytes a workgroup to shared memory
(the driver's "Shared Memory Size"), which comes out of the L1: a build
that lands on 64 registers but spills 5,888 bytes runs no faster, its
extra warps paying for a quarter of the L1, so a change to the resolve is
judged by both numbers. The view's values that every invocation reads
come as the pass's push constants (`NphVisResolvePush`: the target's size,
the cluster, instance and mesh tables' and the render view's addresses,
and the transform), which the compiler uses as constant-bank operands
where values loaded from the view record take registers; and the debug
views have pipelines of their own (`k_debug`), so the others compile
without their branches. The allocation sits on a cliff, each variant
landing differently: the transform comes from the push constants in the
radiance cache's variants and from the view record in the others, which
each measured best. The street's variant holds 64 registers and 3,840
bytes, and the pass took 3.21 ms against 3.36 in the street, 2.73
against 2.85 at the hedge, 1.12 against 1.17 in the 4K field, 0.93
against 0.97 in the field without the cache and 1.10 against 1.22 in the
sunlit field without it (interleaved, three rounds; with every value
from the push constants the cache-less field took 1.02, its variant
spilling 4,352 bytes). Measured and not kept, in runs interleaved on the
street, the hedge view and the 4K field:
- the visible cluster carrying its meshlet's streams (48 bytes instead of
  16), so that the instance, the mesh and the triangle load together and
  the meshlet record is not read (eight dependent loads down to five): the
  resolve 0.03 to 0.05 ms faster, the raster 0.04 slower, 17 MB more held.
  Those records are shared by every lane of a warp and mostly hit the
  caches already;
- each invocation shading 4 or 16 pixels of a 16- or 32-pixel square in
  turn, so that its later pixels find the records and texels in the L1:
  0.35 to 0.47 ms slower;
- the emissive texture sampled with the others before the environment, and
  the radiance cache's lookup split so that its slot alone stays live
  across the material: the compiler's allocation (72 registers, 2,304
  bytes) did not change;
- the ambient occlusion fetched first thing, so that its latency hides
  behind the triangle's: 0.03 to 0.05 ms slower.

**The HDR target** is B10G11R11 where the device can store to it, sample
it, and blend into it (RGBA16F otherwise; the reference keeps RGBA16F):
half the bytes for every pass that reads it. Its mantissas are 6 bits (5
for blue), so the resolve rounds each colour to the format's grid itself,
up or down at random with the odds that keep its expected value
(interleaved gradient noise; Gjøl & Svendsen 2016): the steps show as fine
noise instead of bands, and the stored value is exact whatever rounding the
device would apply (Vulkan leaves it open; the RTX 3060 Ti truncates).
Debug views are stored without the dither.

### Lights

The production path lights from a grid built on the GPU every frame
(`NphRenderLightGrid`; Drobot, "Improved Culling for Tiled and Clustered
Rendering", SIGGRAPH 2017):

| Pass | What it does |
|---|---|
| `vis light updates` | the frame's changed lights into the table |
| `vis lights` | every light made camera relative; directional and unlimited lights to the global list; the others bounded by the sphere around what they reach (a spot light's cone sphere, Wronski 2017), kept when it meets the frustum, given the bins of their nearest and farthest distance and the tiles of their clipped screen rectangle (Mara & McGuire 2013) |
| `vis light bins` | one workgroup: a counting sort of the kept lights by nearest bin into the grid's local list, and each bin's range of sorted lights that may reach it |
| `vis light tiles` | once the frame's depths are final: a bit per light and 16-pixel tile, a word of 32 lights at a time, after a block test per 8 × 8 tiles; a mask for the opaque surfaces, culled against the tile's depth bounds, and one for blended surfaces |

The 1,024 distance bins are spaced evenly in log2 distance over 20
octaves from the near plane; a bin's range runs from the first sorted
light whose running maximum of farthest bins reaches it to the last whose
nearest bin is at most it. A pixel evaluates the global lights, then the
sorted lights in its bin's range whose bit its tile's mask sets, each
invocation on its own (walking the subgroup's lights together, as Drobot
does, measured slower on the RTX 3060 Ti). The bins and tiles are
conservative, so a light they hand a pixel is first tested against the
pixel's distance from its position alone (one 16-byte read), and only a
light within range is read whole and evaluated. When the grid handed
each pixel of Bistro's street 11.8 lights, 5.0 of them within range (before
the tiles' depth bounds, below), the test took its resolve from 3.87 to
3.58 ms, and the 4,000 lamps' from 3.16 to 2.88 ms. Culling is exact: a light left
out contributes zero at the pixel (its windowed falloff, or its cone), the
bounds carrying a 10^-4 slack for rounding; 300 lights around and behind
the camera give the image of lighting every pixel with every light, bit
for bit. `NPH_RENDER_VIS_NO_LIGHT_GRID` makes every light global, for such
comparisons. A view holds at most 4,096 lights (`max_lights`). Blended
surfaces walk the same grid per fragment, with tile masks of their own.

The tile masks are culled against what each tile shows (the tile depth
bounds of Harada, McKee & Yang, "Forward+", Eurographics 2012). The depth
pyramid's passes, which read every pixel's final depth anyway, keep each
16-pixel tile's nearest and farthest surface depths and whether the
background shows (shared-memory atomics per workgroup of 2 × 2 tiles; a
subgroup reduction instead, with each tile's blocks in subgroups of their
own, measured 0.008 ms slower in the pass for its memory order). Once both
phases have drawn, `vis light tiles` keeps a light in a tile's mask for
opaque surfaces only where the sphere around its reach meets the box
around the tile's part of the view between those two distances (Olsson &
Assarsson, "Tiled Shading", JGT 2011, bound tiles by such boxes); a tile
without surfaces keeps none. A second mask serves blended surfaces, which
lie anywhere in front of the opaque ones: the box runs from the near plane
to the farthest surface, and where the background shows the tile keeps
every light whose rectangle covers it. Forward-drawn surfaces walk that
mask (`NPH_SURFACE_GRID_TILES`). The box carries the grid's 10^-4 slack,
so a light left out reaches no surface of the tile and the image does not
change, bit for bit (the image test above). The lights a pixel tests fell
from 11.8 to 5.7 in Bistro's street (5.0 within range), from 10.8 to 4.8
on the terrace (4.4) and from 8.8 to 3.1 at the hedge (2.8). The resolve
took 3.34 ms against 3.53 in the street, 3.25 against 3.45 on the terrace
and 2.84 against 3.02 at the hedge, for 0.03 ms more in `vis hzb 0` and
0.01 ms more in `vis light tiles`: the frame 0.14 ms less. The 4,096
lamps' frame took 4.80 ms against 4.95, its masks 0.18 ms against 0.09: a
light's record is read only by the blocks of tiles its rectangle reaches.
A frame without local lights in view skips the bounds (the pyramid's pass
costs what it did). Scenes that make no use of the grid moved by up to
0.06 ms with the masks' buffers' allocation alone, which a dummy
allocation of the same size reproduced: where later resources land in
memory, not work.

A local light's terms are rearranged exactly (`nph_lit_local`,
`nph_lit_reflect` in surface.glsl): the range test's vector is reused,
one reciprocal square root gives the direction and the falloff, N·H and
V·H come from |v + l|² = 2 + 2 v·l, the distribution and the visibility
share one division, and Schlick's power is three multiplies: four
transcendentals a light instead of nine, the results a few ulps from the
textbook order's (some 10⁻⁷ of a value the B10G11R11 target rounds to
steps of 2⁻⁶ of it). N·L is tested from the position before the rest of
the light's record loads. First measured on 2026-10-20, the compiler gave
it 64 registers and 5,888 bytes of spill and Bistro's hedge took 0.10 ms
more; after the light tiles' depth bounds and the resolve's push
constants (2026-10-21) the resolve took 3.06 ms against 3.20 in the
street, 2.99 against 3.10 on the terrace, 2.66 against 2.71 at the hedge
and 2.32 against 2.52 under the 4,096 lamps (interleaved, two rounds; the
field the same). Loading two candidates' positions together, measured
with it the first time, is not kept.

The terms of the BRDF that no light changes are computed once per pixel,
not once per light (`NphLitPoint`): F0, the diffuse colour over π, the
multiple-scattering compensation 1 + F0 (1/E_ss − 1), α², the
height-correlated Smith visibility's view half √(N·V² (1 − α²) + α²) and
the raw N·V, each by the arithmetic the loop used, so the image does not
change. A local light's record is read through a reference to that one
record (`LightRef(locals)[i]`, GL_EXT_buffer_reference2), whose fields
load at fixed offsets from one address: indexing the array instead
recomputed the 64-bit address for each of the record's 16-byte words,
three integer instructions a load. The test that skips a dark light (its
window or cone zero at the point) reads the attenuation instead of the
illuminance's three channels, the same lights skipped but those of colour
zero, which reflect zero anyway. In GPU traces of Bistro's street (Nsight
Graphics GPU Trace, instruction counts exact to the unit between traces
of the same frame) the resolve issued 2,180 instructions a pixel against
2,291, the reference alone 40 of the difference; with 64 registers and
1 KB more spill, it took 3.07 ms against 3.13 (median of five traces each,
interleaved). The resolve reads its cluster's, instance's, mesh's and
material's records the same way (`VisClusterRef`, `VisInstanceRef`,
`VisMeshRef`, `MaterialRef`; each record a multiple of the references'
16-byte alignment, so indexing one strides by its size): 2,131 to 2,137
instructions a pixel against 2,180, the resolve 2.99 ms against 3.05 (two
traces each).

The surface is filled in the order that ends live ranges soonest: right
after the barycentrics the facing test, the face normal and the
position's interpolation (and the radiance cache's lookup, which keys on
those two), so the clip and world positions end there, and each attribute
interpolated as soon as it is unpacked; filled at the end, they had stayed
live across the attributes' decode. The arithmetic is the same. The
street's variant spills 2,304 bytes a workgroup instead of 3,840, the
radiance cache's alone 1,792, and the shadow map's without the cache
holds 64 registers instead of 72; in the street the resolve took 2.83 ms
against 3.02 to 3.09, its warps 64.5% of the SM's 48 against 61.8% and
its issue 74% against 71% (GPU traces, two each, interleaved), on the
terrace 2.83 against 2.97 and at the hedge 2.49 against 2.74. The other
way: the decode's arithmetic trimmed (the normal through the transform's
cofactors turned by the mirrored flag, the tangent's and the colour's
bytes interpolated before scaling) issued about as many instructions, but
the street's variant spilled 5,888 bytes and the resolve took 3.19 to 3.30
ms. A change here is judged by its spill as much as by its count.

The specular lobe's Fresnel term and its multiple-scattering compensation
come to the loop folded into Schlick's two terms, energy·F0 and
energy·(1 − F0), computed once per pixel, where the loop had multiplied
F0 + (1 − F0) x⁵ by the energy for every light: the same algebra, a light
then one fused multiply-add a channel for both. The instruction count
barely moves (the compiler had already hoisted 1 − F0), but the street's
variant spills 1,792 bytes instead of 2,304 and the radiance cache's alone
none: in the street the resolve took 2.80 and 2.85 ms against 2.87 and
2.90, on the terrace and at the hedge the same within the traces'
spread.

### Temporal anti-aliasing

A production view with `NphRenderView.anti_aliasing` set to
`NPH_RENDER_AA_TEMPORAL` (the runtime's default, `r.taa`) is anti-aliased
temporally at native resolution, for anti-aliasing only (CLAUDE.md
§8.6.8; Karis, "High Quality Temporal Supersampling", SIGGRAPH 2014;
[ADR-0034](adr/0034-temporal-anti-aliasing.md)):

- **Jitter.** Every pass renders with the projection moved by the next of
  eight Halton (2, 3) points, two to each quarter of the pixel. The
  culling's occlusion tests, the pyramid's dirty tiles, and the light grid
  move their projected rectangles by the jitter of the frame they test
  against (`NphVisViewData.jitter`). Debug views and the reference
  renderer are never jittered, and debug lines are drawn after the resolve
  without it.
- **One pass** (`taa.frag`, the arithmetic in `taa_common.glsl`) replaces
  the tonemap: a full-screen fragment pass writing the display colour and
  the next history as two colour attachments. The history is two images
  of the HDR format taken in turn, stochastically rounded with noise that
  moves every frame.
- **Motion.** The nearest surface in the 3 × 3 neighbourhood: four gathers
  of the visibility buffer's depths through its R32G32_UINT sampled view
  (`NphGpuImageDesc.sampled_format`), then one fetch of its record. Its
  point goes from this frame's clip space to the previous frame's without
  its jitter (`vis_previous_uv`, which the motion blur shares): through
  one matrix for a still surface (`NphVisViewData.reproject`: the previous
  view-projection, the camera's move, this frame's inverse), which leaves
  the sky to the camera's rotation, and through the previous transform for
  an instance that moved (`NPH_VIS_INSTANCE_MOVED`, which the scatter pass
  keeps and the meshlet cull copies to the cluster).
- **The blend.** The 3 × 3 neighbourhood filtered with a Gaussian of a box
  filter's spread under the jitter; the history read by five Catmull-Rom
  taps (Jimenez 2016), rescaled by the exposure's change, and clipped in
  YCoCg toward the centre of the neighbourhood's variance box within its
  extremes (Salvi 2016; Pedersen 2016), the box at least 1/32 of its luma
  wide (the history's own rounding); weights divided by 1 + luma (Karis
  2014), the current frame's 1/16 still and up to 1/4 at a pixel a frame of
  motion.
- **The history's life.** It continues only from the frame just before,
  resolved temporally into the same extent, without
  `NphRenderView.camera_cut` (which the runtime sets when it switches
  between the world's camera and the host's); otherwise the frame stands
  alone. The statistics say whether the frame resolved temporally (`taa`)
  and blended with history (`taa_history`).

Against the reference renderer at 4 × 4 the resolution, a still view
converges to 0.39 levels of mean error (one sample per pixel: 1.22), a
panning camera holds 0.65 (1.24), and a moving cube 0.33 (0.62), with no
trail beyond an edge's own pixel. The pass costs 0.75 ms in the 4K field
against the tonemap's 0.11 ms, over the §10.4 line for TAA, post, and the
tonemap together; ADR-0034 records why, and what can bring it down.
Measured and not kept (2026-10-20, three rounds interleaved): the bloom's
tap issued first, and on frames where no instance's record changed the
nearest surface's motion taken from the gathered depth word alone, so that
neither its whole record nor its cluster's flags were read before the
history's taps (four dependent steps down to two, the image bit for bit
the same): 0.859 ms against 0.853 in Bistro's street, 0.838 against 0.830
in the 4K field. The pass is not waiting on that chain. Nor kept
(2026-10-21, two rounds): the neighbourhood fetched with immediate offsets
where the whole subgroup lies a pixel inside the image, the nearest-depth
search tracking one index, and the finite test as one compare: 0.890 ms
against 0.864 in the street. Nor kept (2026-10-21): the tonemapper chosen
by a specialisation constant instead of a push constant (0.004 ms less,
not worth a pipeline per tonemapper). Writing the swapchain through an
sRGB view instead of encoding in the shader would save the encode's
0.03 ms (`VK_KHR_swapchain_mutable_format`), but the hardware's rounding
may differ from the exact curve by one step in the last bit: parked.
Nor kept (2026-10-25, the structure checkpoint, ADR-0059): the pass as a
compute pass taking its neighbourhoods from shared memory, each texel
turned into YCoCg once and the moments, extremes and Gaussian sum taken
along rows and then columns (the same images: 0.865 to 0.872 ms in
Bistro's views against 0.872 to 0.879, and 1.095 in the 4K field against
0.835); and the pass fused into `vis resolve`, whose decisive costs the
checkpoint probed: workgroups of 16 × 16 compile the resolve to 48
registers and 19 KB of spill a workgroup (+0.8 ms), and at its 8 × 8, 44%
of the pixels border another tile.

### Motion blur

A production view with `NphRenderView.shutter` above zero blurs each pixel
over what crosses it while the shutter is open, centred on the frame's
instant: its surface's motion since the previous frame times `shutter /
frame_seconds` ([ADR-0036](adr/0036-motion-blur.md)). A 180-degree shutter
at 240 frames a second is 1/480 s. Only lit frames that follow a
production frame of the same extent without a cut blur (the statistic
`motion_blur`); the runtime gives the world camera's `shutter` and closes
it with `r.motion_blur`.

- **After the temporal resolve.** On such a frame the temporal resolve
  writes only its history (`taa_history.frag`), and the blur reads it and
  writes the target with the tonemap (`motion_blur.frag`): the resolve sees
  sharp surfaces, and no blur reaches the next frame's history. Without
  temporal anti-aliasing the blur reads the HDR image and mixes the bloom
  in itself. The debug lines are drawn after it.
- **Motion** (`motion.comp`): each pixel's, as the temporal resolve takes
  it ("Motion" above), its blur cut to a tile, 32 pixels (31.94 as packed),
  so no blur reaches past the tiles around its own; packed with the view
  distance into a word (`motion_common.glsl`: sixteenths of a pixel, and
  log2 of the distance in 1.1 % steps). Each 32-pixel tile keeps its
  longest blur and the spread of its pixels' around it (McGuire et al.,
  "A Reconstruction Filter for Plausible Motion Blur", I3D 2012).
- **Neighbourhoods** (`motion_tiles.comp`): each tile's dominant blur is the
  longest of its 3 × 3 tiles that can reach it, a diagonal one only if its
  blur runs along that diagonal (Guertin, McGuire & Nowrouzezahrai, "A Fast
  and Stable Feature-Aware Motion Blur Filter", HPG 2014).
- **The gather.** Where every blur in the neighbourhood lies within half a
  pixel, or a tenth of its length, of the dominant one (a turning camera),
  the pixel is the average of the colour along it, by bilinear taps a pixel
  apart (4 to 16). Elsewhere it samples along the dominant blur and, every
  other sample, its own (or the dominant one's perpendicular where it
  barely moves), classifies each sample against itself by a relative depth
  test, and weights them by the shutter: a surface point sweeps its blur's
  segment and covers a pixel on it for 1 / (2 reach) of the time per pixel,
  so samples in front cover the pixel for as long as their sweeps cross it,
  samples behind show through for as long as the pixel's own surface has
  swept away, and samples of its own surface give the colours that pass
  over it; the front layer goes over the pixel's surface over the revealed
  background (4 to 32 samples). Near a tile's edge a pixel takes the tile
  across it at random, so neighbouring tiles' blurs meet as noise, not a
  seam.

Against the shutter's own average (32 sharp frames at instants across it),
a box moving 8 pixels a frame holds 0.73 levels of mean error (the sharp
frame: 4.26), a turning camera 0.59 (4.34), a box moving behind a still one
0.84 (4.53) with the still one untouched, and under temporal anti-aliasing
0.78 (1.67); Guertin et al.'s weights gave 1.83, 0.76, 1.96, and 1.23. At
4K with typical motions it adds 0.51 to 0.60 ms to the frame (the motion
0.23 ms, the blur 0.39 to 0.58 ms), beyond the §10.4 post line (ADR-0036).

### Ambient occlusion

A production view with `NphRenderView.ao_radius` above zero darkens the
environment's light by ground-truth ambient occlusion: the cosine-weighted
share of each pixel's hemisphere that no surface within the radius covers
(Jimenez, Wu, Pesce & Jarabo, "Practical Realtime Strategies for Accurate
Indirect Occlusion", ATVI-TR-16-01, 2016;
[ADR-0038](adr/0038-ambient-occlusion.md)). The occlusion is made for lit
views and the indirect debug view while the environment lights the view
(not with `disable_ibl` or no environment), and for the occlusion debug
view (the statistic `ao`); the reference renderer has none. The runtime
sets the radius from `r.ao` (0.5 m).

- **Full resolution** (the user's decision, 2026-10-20: the paper's
  §4.1 half resolution traded detail at edges and on thin geometry for
  time). The depth pyramid's passes write every pixel's depth (mip 0)
  and each 2 × 2 block's average (mip 1) as they read the frame's depths,
  and `ao depth` averages four more mips of reverse-Z depth, which is
  affine across a plane, so every mip's points stay on the planes they
  come from (XeGTAO's depth mips, Intel 2021). The extent rounds up to 32
  pixels so that every mip halves it exactly and one uv addresses a point
  in all of them.
- **A slice a pixel** (`ao.comp`): its angle around the view vector one
  of 16 over 4 × 4 pixels (a Bayer matrix), turned by a sixth of a step
  each frame under temporal anti-aliasing, which averages the rotations:
  96 directions a pixel. Along the slice's screen direction (the
  projection's derivative, so the samples lie in the slice), 4 steps a
  side out to the radius (at most a quarter of the image's height), at
  least a pixel out: the near ones interpolate mip 0 exactly where its
  texels lie on one plane, which a sample beside the slice would rise
  above at a grazing angle; the steps 16 pixels out and further read
  coarser mips, each texel an eighth of the step's length at most.
  Occluders fade over the last tenth of the radius; a radius under a
  pixel occludes nothing. The horizons give the slice's visible arc in
  closed form (Eq. 7), scaled by the normal projected into the slice
  (Eq. 8; the normal from the depth's gradient). A workgroup takes its
  pixels sorted by direction, so that each half of a subgroup takes one
  direction's pixels, 4 apart, whose samples fall on one lattice
  (deinterleaved, as Bavoil, "Deinterleaved Texturing for Cache-Efficient
  Interleaved Sampling", NVIDIA 2014, but over the frame's own depths);
  and every sample is fetched within the image, one beyond the reach
  counting nothing, so the fetches never wait on a branch.
- **The denoise** (`ao_denoise.comp`): a 5 × 5 tent of pixels, which holds
  each of the 16 directions equally, counting the neighbours on the
  pixel's tangent plane (5 % of the distance off it counts nothing); the
  occlusion is the window's visible shares over the open shares its slices
  would have, so an open surface stays open where an edge cuts directions
  out. Each invocation denoises a 2 × 2 block, whose four windows lie in
  one of 6 × 6 pixels that it reads and unpacks once, adding each texel to
  each window in that window's own order, so every sum is what it would be
  alone; a block of the sky alone is written open at once. A tap's plane
  test takes the plane's inverse distance from a base per row and pixel
  (q + o_y step_y, once a row) plus o_x step_x, one fused multiply-add a
  tap where q + o · step took more: in Bistro's street 315 instructions a
  pixel against 331 and 0.413 ms against 0.439 (GPU traces, two each,
  interleaved), the weights differing only in rounding. The occlusion goes
  out as a half float.
- **Shading.** The resolve reads the occlusion at its pixel. The
  environment's specular light takes specular occlusion from the
  material's occlusion times the screen's, and its diffuse light the
  screen's through Eq. 10 of the paper, which adds back what occluders of
  the surface's albedo reflect (`nph_render_ref_ao_bounce`); direct light
  is untouched.

Against a ground truth cast by rays, a box on the ground is occluded
within a sixth of how much the truth is (a fifth at half resolution), the
ground nothing occludes stays within 0.003 of open with no pixel off by
over 0.03 (0.5 % at half resolution), and the ground alone within 0.009
out to its horizon (`test_ao.c`). At 4K with the runtime's radius it adds
1.06 ms to a frame the sky lights, and its passes take 1.31 ms in
Bistro's street (ADR-0038). Measured and not kept: the denoise unpacking its texels into
vectors in shared memory (0.76 ms against 0.59, the reads three times the
bytes); unpacking the shares with float bit tricks instead of conversions
(0.47 ms against 0.44); workgroups of 32 × 16 pixels whose subgroups each
take one direction whole (0.87 ms against 0.84); every loop of the denoise
unrolled (`[[unroll]]`: 128 registers and 0.59 ms against the driver's own
choice, 64 and 0.44), and the occlusion's steps (0.83 ms against 0.85, 40
registers against 38: within two rounds' noise); the occlusion's mip 0
read from the visibility buffer instead of the copy the depth pyramid's
pass writes (2026-10-21: the pyramid 0.03 ms less, the occlusion 0.065 ms
more, in Bistro's views); the denoise filtering 2 × 1 pixels an invocation
(2026-10-21: 48 registers against 64, yet 0.49 ms against 0.44: its texel
reads, not its occupancy, bound it); the resolve denoising its own pixel
as it begins, the pass gone (2026-10-25, ADR-0059: the same images, but
the resolve grew by 0.58 to 0.64 ms in Bistro's views against the pass's
0.43, the window's 25 fetches delaying the chain of loads behind them).

### Global illumination

A production view with `NphRenderView.gi`, on a device with ray queries,
takes its diffuse indirect light from a radiance cache in the world
instead of the environment's irradiance alone: the environment's light
occluded by the scene, and the sun's, the environment's, and emissive
surfaces' light the scene reflects, bounce after bounce (a hash grid after
Binder, Fricke & Keller, "Fast Path Space Filtering by Jittered Spatial
Hashing", SIGGRAPH 2018 talks, and "Massively Parallel Path Space
Filtering", MCQMC 2020; Gautron, SIGGRAPH 2020 talks;
[ADR-0039](adr/0039-radiance-cache.md)). The cache is made for lit views
and the indirect and irradiance debug views (the statistic `gi`); the
reference renderer has none. The runtime turns it on with `r.gi`.

- **Cells** (`gi_common.glsl`): a cube of the world at a level the
  point's distance from the camera chooses, 1/96 to 1/48 of that distance
  across (23 to 47 pixels at 4K), for the surfaces in it whose geometric
  normals face one octant, so the two sides of a wall are cells apart.
  Each level's camera cell is worked out in double precision and the
  shaders add the camera-relative point to it, so cells stay put in the
  world at any distance from the origin.
- **The table:** `gi_cells` slots (2^18: 9 MB) in buckets of 16; a cell's
  key (its coordinates, level, and octant) hashes to a bucket and a 32-bit
  fingerprint that the slot holds, claimed by compare-and-swap.
- **`gi update`** (`gi_update.comp`): within `gi_rays` (65,536) a frame,
  one ray from a random pixel of each block of the frame (12 × 12 pixels
  at 4K): the surface point rebuilt as the resolve rebuilds it (a voxel
  pixel's as the voxel resolve finds it, "Voxels", in a variant
  specialised for frames that may draw voxels), its cell claimed, one
  cosine-distributed ray traced. A miss adds the
  environment's radiance; a hit its emission and its albedo times the
  irradiance there, the sun's (shadowed by a second ray where it casts
  shadows) and the cache's own (the environment's where it has none), so
  each frame adds a bounce. Hits within 0.95 of the ambient occlusion's
  radius count nothing: that range is the occlusion's.
- **`gi resolve`** (`gi_resolve.comp`): every slot blends its frame's
  samples into its irradiance over a history of at most 128 samples, or
  ages, and is freed after 240 frames without any.
- **The lookup:** the resolve, specialised on the cache, finds the
  pixel's cell as soon as it has the pixel's point (under temporal
  anti-aliasing at a random point of the cell in the surface's plane, so
  that the cells' edges blend over frames) and the shading takes the
  cell's irradiance, the environment's making up for what a cell does not
  trust yet. Within the ambient occlusion's radius the occlusion darkens
  it as it darkens the environment's.

Against ground truths cast by rays, an open ground under a uniform sky
gets the sky's irradiance exactly, a box on the ground occludes the sky
within a tenth of how much the truth does, and a sunlit wall reflects
light onto the ground within a seventh of the truth (`test_gi.c`). At 4K
it adds 0.20 to 0.27 ms under the sky or the sun, and 0.36 to 0.41 ms at
night among many lamps (ADR-0039).

### Capacities and statistics

The production path's lists have fixed capacities in `NphRendererDesc`:
`max_visible_clusters` (524,288), `max_cull_meshlets` (2^20, phase 0's
meshlet work items), `max_deferred` (262,144: deferred instances, and
phase 1's meshlets), `max_instance_updates` (16,384),
`max_material_slots` (4 × `max_instances`), and `meshlet_index_pool`
(2^24, indirect path only), and `max_lod_nodes` (2^18 work items a step of
the walk down the levels of detail, at most 524,224); `max_lights` is at most 4,096, and
`max_cells` (4,096) at most 2^24 - 1 (a record names its cell in 24 bits). A list that fills drops work and sets a bit in
`NphRenderFrameStats.vis_overflow`, logged once. The sun's map has
`vsm_pages` (2,048), `max_vsm_meshlets` (2^20), `max_vsm_draws` (2^20)
and `max_vsm_voxel_draws` (2^20): the pool's overflow (`vsm_overflow`)
raises its resolution bias, and the lists set its page budget, which bounds
what a frame draws ("Shadows"); its statistics are `vsm` and
`vsm_lod_bias` (this frame) and the pages requested, wanted, drawn, held,
and evicted, the page budget (`vsm_budget`), the casters' meshlets tested
(`vsm_meshlets`, what `max_vsm_meshlets` must hold) and the nodes their
walk visited (`vsm_nodes`), and the caster draws, read back as the others. The radiance cache has `gi_cells` (2^18) slots, a
bucket of which, when full, drops the samples of the cells it cannot
take, and `gi_rays` (65,536) a frame. The frame statistics
carry the GPU's counters (clusters and triangles drawn, clusters drawn in
software, instances and meshlets culled by each test, the hierarchy's
nodes visited and the clusters a finer level stood in for, lights in view,
cells drawn as their proxies and the instances they stood in for),
read back from the frame that completed
`NPH_GPU_FRAMES_IN_FLIGHT` frames earlier (`vis_frame`). On this path
`instances` is the live count and `culled` the hidden instances plus
blended ones outside the frustum; `draws` and `triangles` count only the
blended surfaces the CPU draws.

### Cost

`render_perf` on the RTX 3060 Ti (Profile; docs/benchmarks.md), the CPU
time of `nph_render_frame` and the GPU frame:

| Scene | Instances | CPU, reference | CPU, production | GPU, reference | GPU, production |
|---|---|---|---|---|---|
| grid, 512 × 512 | 1,024 | 0.20 ms | 0.010 ms | 0.10 ms | 0.30 ms |
| grid, 512 × 512 | 10,240 | 1.26 ms | 0.010 ms | 0.22 ms | 0.36 ms |
| grid, 512 × 512 | 102,400 | 13.2 ms | 0.010 ms | 3.13 ms | 0.72 ms |
| field, 3840 × 2160 | 16,385 | 2.87 ms | 0.012 ms | 3.06 ms | 3.55 ms |
| the same without temporal anti-aliasing | 16,385 | | 0.012 ms | | 2.72 ms |
| the same without bloom | 16,385 | | 0.012 ms | | 3.19 ms |
| the same without ambient occlusion | 16,385 | | 0.012 ms | | 3.16 ms |
| the same without global illumination | 16,385 | | 0.010 ms | | 3.27 ms |
| the same exposed automatically | 16,385 | | 0.013 ms | | 3.55 ms |
| the same, motion blur with the camera turning 60°/s | 16,385 | | 0.013 ms | | 4.15 ms |
| the same, strafing at 5 m/s | 16,385 | | 0.013 ms | | 4.12 ms |
| the same, turning 360°/s | 16,385 | | 0.013 ms | | 4.03 ms |
| the same, depth of field focused at 5 m (circles to 4 px) | 16,385 | | 0.013 ms | | 4.55 ms |
| the same, focused at 2.6 m (circles to 7.7 px) | 16,385 | | 0.013 ms | | 5.12 ms |
| the same, focused at 0.6 m (circles at the 32-px limit) | 16,385 | | 0.013 ms | | 4.91 ms |
| field at night, 1,024 lights | 16,385 | 3.61 ms | 0.012 ms | 53.0 ms | 3.48 ms |
| field at night, 4,000 lights | 16,385 | | 0.012 ms | | 4.99 ms |
| field, the sun casting shadows (production: its map) | 16,385 | 3.20 ms | 0.014 ms | 3.69 ms | 3.82 ms |
| the same without global illumination | 16,385 | | 0.011 ms | | 3.70 ms |
| the same, the sun's shadows ray traced | 16,385 | | 0.012 ms | | 3.91 ms |
| the map, a sphere moving every frame | 16,386 | | 0.014 ms | | 4.18 ms |
| the map, the camera walking | 16,385 | | 0.014 ms | | 3.83 ms |
| field at night, 1,024 lights and the moon casting shadows | 16,385 | | 0.014 ms | | 4.77 ms |

Every frame draws the runtime's bloom (0.04); the production path's
frames resolve temporally (the runtime's default), take global
illumination (`r.gi`), and, where the sky lights them, ambient occlusion
within 0.5 m (`r.ao`), the reference's none of these; the motion-blurred
frames have a 180-degree shutter at 240 frames a second, and the others
with depth of field a full-frame lens at f/1.4 (`render_perf-2026-10-20d`;
a frame varies by a few hundredths of a millisecond from run to run).

In the 4K field the surviving clusters hold 5.4 million triangles (the
reference draws 14.6 million after its frustum test; occlusion removed
52,249 clusters, and 57,587 of the 64,128 drawn went to the software
raster), and the passes take: clear 0.08 ms, culling with its argument
passes 0.09 ms, raster 0.49 ms (0.20 in hardware, 0.29 in software), the
two pyramids 0.20 ms (0.18 without the ambient occlusion's depths), the
light grid 0.014 ms, the ambient occlusion 0.30 ms, the radiance cache's
update and blend 0.043 ms, resolve 1.22 ms (0.99 without the radiance
cache's lookup, and 0.06 less again without the ambient occlusion), the
bloom chain 0.28 ms, the temporal resolve with the glare and the tonemap
0.82 ms (0.75 without bloom; the tonemap with the glare: 0.13 ms).
Against CLAUDE.md §10.4, the raster (0.65 ms budgeted) and the shading
(1.20 ms; the resolve without the cache's lookup) fit; culling with the
depth pyramid takes 0.29 ms against 0.25 ms, most of it the first
pyramid's read of the visibility buffer; the ambient occlusion takes
0.38 ms (its share of the pyramids and the resolve included) and the
radiance cache 0.27 ms (its lookup included) of the indirect light's
0.60 ms: 0.65 ms together (ADR-0038, ADR-0039); the temporal resolve and
the bloom chain (1.10 ms) are past the 0.40 ms of TAA, post, and the
tonemap together (ADR-0034, ADR-0035); motion blur adds about half a
millisecond when on (ADR-0036), and depth of field 0.97 to 1.54 ms
(ADR-0037). With 1,024 lamps the grid's passes take 0.050 ms and the resolve 1.50 ms, 1.20
of it without the radiance cache's lookup (every light for every pixel:
49.3 ms), at the shading line.
The sun's virtual shadow map adds 0.35 ms to a still frame without global
illumination (0.28 with it), at the shadow line (0.40 ms); its rays add
0.36 ms still. A frame in which an instance moved rebuilds the top level
wherever rays are traced, which global illumination's are on every
frame: 0.36 ms with its records (over 102,401 slots; 0.22 ms over the
scene's own 16,386), so a sphere moving every frame costs the map's frame
0.64 ms. The lamps' shadows (rays) add 1.20 ms. ADR-0029 to ADR-0033 and
ADR-0039 record the numbers and what comes next.

### Where the time goes

GPU traces of Bistro's views (Nsight Graphics GPU Trace on the RTX 3060
Ti, driver 621.01, 2026-10-21; steady frames of the street, terrace and
hedge views) give each pass's unit throughputs, its warps' stall reasons
and the instructions it issued. The frame is bound by instruction issue
under the board's power limit, not by memory: through the frame the GPU
draws 199.8 of its 200 W at 1.71 to 1.77 GHz (the clock follows the
work's power; the throttle reason is the software power cap), and the
expensive passes issue an instruction on 72 to 89% of their cycles while
their DRAM traffic is 10 to 35% of its peak:

| Pass (street) | Instructions a pixel | Issue | DRAM | First stall reasons |
|---|---|---|---|---|
| `vis resolve` | 2,291 | 74% | 16% | memory latency 34%, ready but not issued 20% |
| `taa` | 782 | 89% | 35% | ready but not issued 40% |
| `ao` | 672 | 78% | 18% | memory latency 37% |
| `ao denoise` | 331 | 73% | 23% | ready but not issued 37% |
| `vis hzb 0` | 278 | 72% | 57% | memory latency 20% |
| `vis raster 0` | 38 | 8% | 71% | memory latency 66% |

The frame issues 4,635 instructions a pixel; at 1.75 GHz the GPU issues at
most 4,290 a pixel in 4.17 ms, so the frame's instructions bound it
before its bytes do. Every instruction saved also lowers the power drawn,
which raises the clock for the rest of the frame.

Instruction counts in a trace are exact (the same to the unit between
traces of the same frames), where times vary by about 2% from trace to
trace and with the clock, so a pass's parts were costed by removing each
in turn and comparing counts. In the street's resolve (2,291 a pixel):
the local lights 697 (about 120 a light, 5.7 lights a pixel), the
triangle's decoding and its attributes 615, the image-based and
radiance-cache light 402, the material's textures 209, the radiance
cache's lookup 193, the sun's shadow map 132, the visibility buffer's
read and the store 49. Against the resolve's 1.34 µs for each instruction
a pixel, removing the lights saved less a part (697 instructions, 0.60
ms), the indirect light as much (402, 0.40 ms), and the textures and the
shadow map more (209, 0.32 ms; 132, 0.19 ms): those wait on memory. The
lookup's 193 cost nothing (removed, the resolve took 0.07 ms more): they
run while the shader waits. In the temporal resolve (782): the history 485 (the
clip and the neighbourhood's moments it needs 203, the Catmull-Rom filter
85), the eight neighbours 228, the glare 83, the tonemap 67 (no time:
hidden), the nearest depth 43, the history's dither 33. In the occlusion
(672): its steps 377, 42 a sample, as the lanes of a subgroup sample
different mips (the step's length scales with each pixel's radius), so
the subgroup runs both `ao_depth_at` paths; the denoise's 25 taps 228,
their plane test 118 of it. In the depth pyramid's first pass (288 in its
trace): the shadow map's page requests 187 (removed, 0.108 ms less), the
light tiles' depth bounds 29, the occlusion's depths 26; the pyramid
alone, 57, takes 0.162 ms, its read of the visibility buffer bound by the
bus. The requests now ask for a 2 × 2 block at once (Shadows): 74 a
pixel.

Measured and not kept:

| Change | Instructions | Time |
|---|---|---|
| A workgroup's triangles set up once each into shared memory (Schied & Dachsbacher 2015's triangle cache, kept per 16 × 16 tile: a tile of Bistro holds 8 to 15 triangles, 17 to 33 pixels each), each pixel interpolating from there | 2,066 against 2,291 | resolve 5.39 ms against 3.16: 80 registers and 33 KB of shared memory leave 37% of the warps active, and the setup's chain of dependent loads (cluster, instance, mesh, meshlet, vertices) waits behind barriers in a few lanes where every pixel's own chain hid it; 8 × 8 tiles: 2,269, 4.64 ms |
| The denoise in packed half precision (two pixels a lane) | 331 → 275 (unrolled; 532 rolled) | 0.44 → 0.46 to 0.48 ms: 80 registers |
| Buffer references declared `readonly restrict` | −2 | no change |
| The SH coefficients' block std430 (its loads aligned to 16) | 0 | no change |
| The temporal resolve's divisions as reciprocals | +7, −1 | no change: the compiler already multiplies |
| The light loop's sum started at the point's indirect and emitted light, so that sum does not stay live across the loop | −22 to −40 | the street's variant spilled 1,280 bytes against 1,792, yet the resolve took 2.82 to 2.91 ms against 2.80 to 2.82 in the street and 2.49 to 2.52 against 2.46 at the hedge (2.77 against 2.80 to 2.82 on the terrace) |
| The radiance cache's lookup after the attributes' decode instead of after the positions, where less is live | +61 to +74 | the variant held 72 registers and spilled 6,400 bytes against 64 and 1,792: the resolve took 3.61 to 3.71 ms against 2.82 to 2.85 in the street, 3.13 to 3.15 against 2.44 to 2.46 at the hedge |
| The transform from the push constants in the variants without the radiance cache too (as in its own) | −2 to −8 | the shadow map's variant held 72 registers and 5,888 bytes against 64 and 6,400; the 4K fields without the cache the same within the traces' spread |

### Debug views

`NPH_RENDER_VIEW_CLUSTERS`, `_TRIANGLES`, and `_INSTANCES` colour each
pixel by a hash of what drew it.

## Image-based lighting

Everything is computed on the GPU in render-graph compute passes, once per
environment. Each pass declares exactly the levels it reads and writes, so
the graph places every barrier.

- **Source** → environment cube level 0 (RGBA16F, 256² by default). The
  source can be the procedural gradient sky, an equirectangular texture
  (u = 0.5 looks down −Z, u grows towards +X, v = 0 is up), or a cube
  texture. A texture source is read from the level whose density matches
  the cube's, so it is not undersampled.
- **Mip chain:** 2×2 box filter per face, one pass per level.
- **Irradiance:** band-2 spherical harmonics (Ramamoorthi & Hanrahan
  2001), projected from the 32² level with exact texel solid angles. One
  workgroup reduces in a fixed order, so the result is deterministic. The
  coefficients are cosine-convolved (Â = π, 2π/3, π/4) and normalised to
  4π.
- **Prefiltered specular:** six roughness levels (128² down to 4²),
  roughness = level / 5. Each texel takes 256 GGX samples under the
  N = V = R assumption (Karis 2013), and each sample reads the environment
  at the level that matches its solid angle (filtered importance sampling,
  Křivánek & Colbert 2008).
- **DFG table:** 128² RGBA16F, F0 scale and bias against (NoV, roughness),
  1024 Hammersley samples.

The sun is not part of the environment: a sun disk at physical radiance
(about 10⁹ cd/m²) does not fit in half floats, and the sun is a
directional light anyway.

## Tonemapping

`tonemap.glsl` maps pre-exposed colour to the display, in the tonemap pass
(`tonemap.frag`) and in the production path's temporal resolve:

- **ACES:** Stephen Hill's fit of the ACES 1.0 RRT and sRGB ODT
  (BakingLab, 2016), with the input and output matrices.
- **AgX:** Troy Sobotka's AgX with the base contrast look: Blender's
  inset/outset matrices and Benjamin Wrensch's sixth-order polynomial fit
  of the sigmoid (2023).
- **None:** a clamp.

On UNORM targets the shader applies the exact sRGB curve; sRGB targets are
encoded by the hardware. Tests compare both against the C reference, texel
by texel, within one 8-bit step.

## Depth of field

A view whose `NphRenderView.aperture` is above zero sees through a thin
lens of that f-number, focused `focus_distance` metres away, in front of a
sensor `sensor_height` metres tall (0: full frame, 0.024 m), whose focal
length follows from the camera's field of view
(`nph_render_view_focal_length`; [ADR-0037](adr/0037-depth-of-field.md)).
Both pipelines blur lit views (the statistic `dof`); the runtime gives the
world camera's lens and makes it a pinhole with `r.dof`.

- **The circle of confusion.** A point z metres away blurs into a disc of
  radius K (1 − z_f / z) pixels, negative in front of the focus, with
  K = f² H / (2 N (z_f − f) h) for an image H pixels tall (Potmesil &
  Chakravarty, "A Lens and Aperture Camera Model for Synthetic Image
  Generation", SIGGRAPH 1981): one multiply-add from the reverse-Z depth.
  Circles are cut at 32 pixels, a tile.
- **Where.** After the scene, before bloom (which glows from the blurred
  image) and the final pass, whose temporal resolve averages the gathers'
  samples as they turn from frame to frame.
- **Setup** (`dof_setup.comp`): every pixel's depth in half floats; the
  half-resolution image, each texel its 2 × 2 pixels' average colour and
  the nearest of their depths; each 32-pixel tile's nearest and farthest
  circle.
- **Tiles** (`dof_tiles.comp`): a tile's 3 × 3 neighbourhood bounds every
  circle that can reach its pixels, which classes it as in focus (all
  under half a pixel), slightly out of focus (none over
  `NPH_RENDER_DOF_SLIGHT`, 8 pixels), or further.
- **The gather** (`dof_gather.glsl`): scattering as a gather (Jimenez,
  "Next Generation Post Processing in Call of Duty: Advanced Warfare",
  SIGGRAPH 2014) with the lens's own weights. The samples lie on a spiral
  turned by the golden angle at equal-area radii, each standing for an
  equal share of the disc, turned per pixel by interleaved gradient noise
  that moves every frame. Against the pixel's own depth (5 % apart is
  wholly in front), samples in front reach the pixel where their circles
  cover it, 1 / (π c²) of their light per unit area: the near layer, its
  coverage in alpha. The others, within the pixel's own circle, average
  into the far layer: its surface and what the circle reveals behind it.
- **Further out of focus** (`dof_gather.comp`): those tiles, and the tiles
  beside them, at half resolution, a sample per 2 square texels of the
  disc, 8 to 48.
- **The resolve** (`dof_resolve.comp`), at full resolution into a new HDR
  image that the rest of the frame reads: tiles in focus are copied as
  drawn; tiles slightly out of focus are gathered at full resolution from
  their pixels and 8 more around them in shared memory, a sample per 8
  square pixels, 8 to 48, where half resolution would carry an edge's
  light a pixel or two past the lens's (as the "slight out of focus"
  gather of Abadie, "A Life of a Bokeh", SIGGRAPH 2018); the rest are
  composited. There a pixel in focus keeps its colour and one out of focus
  takes the far layer, with the near layer over either, from the four
  half-resolution texels around it, those of another surface (1 to 5 %
  apart in depth) all but left out, and by two bilinear fetches where all
  four are of its surface. Beside a blurred surface in front, that
  surface's texels' own colour carries its blur's edge on across the sharp
  edge under it; a sharp surface in front leaves the pixel as drawn.

`NphRenderView.lens_offset` sees a frame through a point on the lens
instead of its centre (the camera moves there and the projection shears,
so the focus plane stays put): averaged over the aperture's disc, such
frames are a thin lens's exact image (Haeberli & Akeley, "The Accumulation
Buffer", SIGGRAPH 1990), for captures and the tests' ground truth. Against
64 of them, a box 10 m away behind one in focus at 3.7 m (circles of 3.8
pixels, the sky's 6.1) holds 2.05 levels of mean error (the pinhole frame:
7.12), the reference renderer 1.98 (7.10), the same temporally resolved
1.18 (6.13), and a box 1.35 m away blurred over a wall in focus (circles of
12 pixels) 1.14 (9.35). At 4K a full-frame lens wide open (f/1.4) adds
0.97 ms with circles up to 4 pixels, 1.54 ms up to 7.7 (the most the
full-resolution gather takes), and 1.32 ms at the 32-pixel limit, past the
§10.4 post line (ADR-0037).

## Bloom and exposure

On both pipelines, after the HDR image is complete and before the final
pass ([ADR-0035](adr/0035-bloom-and-exposure.md)):

- **Bloom** (`NphRenderView.bloom`, the image's share of glare, 0 to 1;
  the runtime's `r.bloom` is 0.04) is a chain of up to six mips, the
  first half the frame's size, each halving the one before exactly
  (`bloom.comp`; Jimenez, "Next Generation Post Processing in Call of
  Duty: Advanced Warfare", SIGGRAPH 2014). Down the chain, each mip is the
  13-tap filter of the one before, five overlapping 2 × 2 boxes of
  bilinear taps; the first weights each box by 1 / (1 + luma) (the partial
  Karis average) so a sub-pixel highlight does not pulse as the jitter
  finds and loses it. Back up, coarsest first, each mip adds a 3 × 3 tent
  of the next, so mip 0 ends as the sum of the levels' blurs. A downsample
  invocation writes 2 × 2 texels from the 25 taps they share, and the
  tent's nine taps fold into four with the same sum. The chain is
  B10G11R11 where the device can store to and filter it, stochastically
  rounded as the HDR image is.
- **The composite** is `mix(colour, mip0 / levels, bloom)`, a lerp that
  keeps the image's energy: in the tonemap pass, and in the temporal
  resolve before the history meets the frame, so the history carries the
  glare and steadies it (applied to the neighbourhood's statistics, the
  glare being the same across it).
- **The automatic exposure** (`NphRenderView.exposure =
  NPH_RENDER_EXPOSURE_AUTO`; `r.auto_exposure`): the first downsample
  counts each half-resolution texel's luminance into a 128-bin histogram
  of log2 pre-exposed luminance over 16 stops (`exposure.comp` then takes
  the mean between the 50th and 95th percentiles, so neither the dark half
  of the frame nor its highlights steer it). The CPU reads it back
  `NPH_GPU_FRAMES_IN_FLIGHT` frames later, undoes the exposure it was
  metered at, and takes the EV100 an averaging meter gives it, log2(L ·
  100 / 12.5) (Lagarde & de Rousiers 2014), less
  `exposure_compensation` stops, within [−6, 20]. The exposure moves
  toward it by `1 − exp(−frame_seconds · exposure_speed)` of the gap a
  frame (defaults 1/240 s and 1.5 a second), starting from the view's
  `ev100`. A metering beyond the histogram's range clamps there, so a
  larger jump takes a few meterings.
- **Scope.** Debug views are neither bloomed nor metered; their frames
  keep the exposure moving toward the last metering. A manual view resets
  the adaptation. The statistics carry the frame's `ev100`, the latest
  `metered_ev100` (0 before one), and `bloom`.

At 4K the chain costs 0.28 ms, mostly its first downsample (0.13 ms,
bound by its 41.5 MB of traffic), and the glare's tap 0.07 to 0.09 ms in
the temporal resolve; the metering adds under 0.01 ms.

## Debug draw

Immediate-mode lines, valid for one frame: AABBs, oriented boxes, spheres,
axes, arrows, crosses, grids, and camera frusta. Appending reserves a slot
with one atomic add, so job workers may draw, but not while
`nph_render_frame` runs. Three upload buffers rotate (frames in flight + 1),
so the buffer being written is never one the GPU is reading. Coordinates
are relative to a 64-bit debug origin, one per frame: the rotation takes
the origin current at that moment for every line of the frame, so the
owner of the frame sets it once and code that only draws reads it with
`nph_render_debug_origin`. Each of the two lists (depth-tested, overlay)
holds `debug_lines` lines; extra lines are dropped and counted.

A frame loop that lets jobs draw lines while it renders (the runtime's
pipelined loop runs the simulation of frame N + 1 during frame N's
submission) closes the list with `nph_render_debug_flip` at its sync
point, after `nph_gpu_frame_begin`: lines added after the flip go to the
next frame, and the writers move to the buffer of frame N − 2, which
`nph_gpu_frame_begin` has already waited for.

## GPU profiler

`NphRenderGpuProfiler` reads `nph_gpu_rg_timings` every frame and keeps a
240-frame window per pass name. It reports mean, min, max, and
nearest-rank p99, along with the pass's **declared footprint**: bytes read
and written as declared to the render graph (§10.7 asks every renderer PR
for milliseconds and DRAM bytes). The footprint is what a pass asks for,
not measured traffic. Caches and framebuffer compression make real traffic
smaller, and repeated reads are not counted. After the passes come two
totals: the frame (`frame`, the sum of the passes) and its span (`span`,
from the first pass's beginning to the last one's end, on any queue, from
the render graph's `begin_ns`). They agree while one queue runs every pass
(Bistro: 7.00 against 6.99 ms); passes overlapping on the async compute
queue would make the span the shorter, and it is the frame time §10.1
counts. Moving the radiance cache's update and resolve there (every image
made concurrent for the trial) lengthened Bistro's span to 7.15 ms and its
resolve by 0.08 ms: the queues' semaphores left gaps, and nothing
overlapped enough to pay for them. Overlapping frames fared no better
(a probe, 2026-10-21): on the async compute queue the resolve alone took
3.67 ms against 3.19 in Bistro's street (the 4K field's was unchanged),
and the next frame's phase-0 raster replayed beside it overlapped but
slowed the resolve by about its own cost, hiding some 0.1 of its 0.49 ms
(the render graph's cross-queue waits, docs/gpu.md, shaped the probe).
The profiler logs a table and writes JSON for the benchmarks.

CPU time: `nph_render_frame` is the profiler zone `render.frame`
([profile.md](profile.md)); the passes it declares are recorded under the
render graph's zones.

## Testing

`render_tests` (registered with CTest as the eight shards `render.0` to
`render.7`, docs/ntest.md) run on the RTX 3060 Ti locally
and on the self-hosted runner, and on lavapipe in CI, under the validation
and synchronisation-validation gate:

- **CPU:** GGX normalisation, Smith symmetry and bounds, DFG energy and
  limits, tonemap ranges and the ACES value at middle grey (worked by
  hand), sRGB round trips, cube-face layout, texel solid angles tiling the
  sphere, SH of a constant environment (π·L), and every primitive's
  winding, tangent handedness, and quantisation.
- **GPU against CPU:** the tonemap curves on UNORM and sRGB targets; the
  DFG table; the SH projection; constant environments through every filter
  (sky, equirectangular, cube); BRDF radiance under directional, point, and
  spot lights; image-based lighting energy conservation; textures and
  debug views; normal maps on mirrored and scaled instances (uniformly by
  100, and unevenly), three- and two-channel, through both pipelines;
  sidedness, alpha modes, blend order, culling; camera-relative rendering;
  debug lines; and the profiler's windows and declared bytes.
- **The production path** (`test_vis.c`): its images against the
  reference's (plain, with every alpha mode and side, and 10,000 km from
  the origin); culling switched off changes nothing; the frustum culls
  what is off screen; mesh shaders and indirect draws give the same image,
  and so do the software and hardware rasters, with no cracks between
  their triangles; instances move, hide, and go, and a whole population is
  replaced between frames; the debug views; cooked and built meshlets draw
  the same; the culling math, the light spheres, the clipped projections,
  and the distance bins against their C twins; the depth pyramid read
  back, and the one after phase 1 against a full rebuild; the light grid
  read back (every light in every tile and bin its sphere reaches, none
  more than a tile beyond); 300 lights giving the image of every light
  everywhere, and lights moving, changing, and going between frames.
- **Levels of detail** (`test_cluster_lod.c`, on the CPU; `test_vis_lod.c`):
  the simplifier's rules (a flat interior collapses to its locked border
  with no error, a curved surface simplifies within its error and with no
  triangle turned over); the levels halve down to a root; every cluster
  has one maker and one consumer; errors never fall and spheres hold their
  parts exactly; a coarse level stays within its error of the true
  surface; every cut of a closed mesh is closed; seams and material
  borders keep their shape; the build is deterministic, the same built on
  jobs (`test_voxel.c`), and degenerate, scattered, small, and bad input is
  handled. Cell proxies (`test_cell_proxy.c`, on the CPU): a proxy's
  records are a voxel-only mesh's levels; what it stops is what its
  instances' cuts stop; what a closed shell or occluders hide is left out,
  and the seen cells' neighbours added and those alone; a turned sheet faces
  where it faces; the same cell gives the same bytes; bad input is refused.
  On the GPU: the clusters
  drawn are the CPU's cut, cluster for cluster, for views that mix levels;
  a threshold of 0 draws level 0 as the mesh without levels; distance
  draws coarser levels; cuts mixing levels leave no hole in a terrain
  sheet and a torus on both raster paths and both rasters; an instance
  revealed in phase 1 draws its whole cut; a full queue is reported; and
  broken levels are refused.
- **Shadows** (`test_shadows.c`), on both pipelines: every pixel is
  classified from the scene's geometry (rays through the corners of its
  footprint, grown by half a pixel, against the floor, the occluder's box,
  and the light), and away from the shadow's edges the shadowed floor is
  black and everything else is as it was without shadows. A cube under the
  sun; a point light's ray that stops at the light (an occluder above the
  light casts nothing); the structures following instances that move,
  hide, and go, and meshes created later; the production path's top level
  kept while the camera moves within 16 m (and the shadows right from
  there) and rebuilt when it must be; rays traced only while a light casts
  shadows; and a renderer without ray tracing drawing such lights
  unshadowed.
- **The sun's virtual shadow map** (`test_vsm.c`), held to the same
  classification with a margin of its texels: the sun behind a cube, with
  mesh shaders and through the indexed indirect raster lavapipe takes; a
  blended floor with nothing opaque under it, shadowed by a cube out of
  view (its pages come from the blended surfaces' own requests); a narrow
  view from 20 km, beyond the window of the level matching its pixels;
  the sun passing between directional lights as they come, change, and
  go; pages drawn once and then kept; a moving caster redrawing its own
  pages, and a far one none; a camera move reusing the pages it had; the
  sun turning redrawing everything; a full pool coarsening the levels
  until they fit, and the bias falling once there is room; agreement with
  the rays it replaces (98 % of pixels within two levels); the scene
  10,000 km from the origin; and a torus with levels out of view, whose
  shadow drawn at the map's cut is its level-0 shadow away from the edges,
  inside and out, from fewer draws, turning the levels on redrawing every
  page (with three of the cut's clusters dropped, pixels inside differ).
- **Bloom and exposure** (`test_post.c`): the metering's mean between its
  percentiles and the EV100 it gives, against the shaders' arithmetic; a
  uniform image kept by bloom on both pipelines and under temporal
  anti-aliasing; a highlight's glow alike in every direction, falling with
  distance, the image's energy kept; the automatic exposure settling on an
  averaging meter's EV100 on both pipelines, with its compensation, and a
  manual view taking its own again; debug views neither bloomed nor
  metered.
- **Temporal anti-aliasing** (`test_taa.c`): the jitter (eight points, two
  to each quarter of the pixel, repeating) and the filter's weights
  against it; a still view, a camera panning across cubes at two depths,
  and a moving cube against the reference renderer at 4 × 4 the
  resolution (the error at most half or six tenths of one sample per
  pixel's, nothing two pixels beyond an edge); a cut giving, bit for bit,
  the frame without history in the same jitter phase; the history's
  continuity across modes and extents; debug views and the reference never
  jittered; an exposure change rescaling the history.
- **Depth of field** (`test_dof.c`), against the average of 64 frames
  through points spread over the lens: a box behind the focus, a near box
  over it, the same on the reference renderer, and a temporally resolved
  view, each well under the pinhole frame's error; tiles in focus as far
  as any circle reaches left as drawn, bit for bit, and a sharp card's
  edges kept bit for bit in tiles further out of focus; the lens offset
  moving a card on the focus plane by nothing and one beyond it by its
  parallax; a pinhole frame the plain one; debug views sharp; lenses that
  cannot focus, or are not finite, refused.
- **Motion blur** (`test_motion_blur.c`), against the shutter's average of
  32 sharp frames at instants across it (supersampled under temporal
  anti-aliasing): a moving box, a half-open shutter (against its own
  average and not the whole frame's), a turning camera, a box moving
  behind a still one (every pixel of the still one kept), and the blur of a
  temporally resolved frame, each well under the sharp frame's error; a
  blur cut at a tile, with a still box beyond it untouched bit for bit; no
  blur on a first frame, a cut, a debug view, the reference renderer, or
  after another pipeline's frame or another extent, and a closed shutter's
  frame the plain one; shutters that are negative or not finite refused;
  debug lines over a blurred frame.
- **Global illumination** (`test_gi.c`), against ground truths cast by
  576 stratified rays a pixel: an open ground under a uniform sky within
  a level of the environment's irradiance; a black box's occlusion of the
  sky within a quarter of how much the truth is, with no more than a
  twentieth of bias; a sunlit white wall's light on a black ground within
  a fifth of the truth; after one frame, every slot holding a cell of the
  view's pixels as the CPU twins of the keys work them out, and nearly
  every such cell a slot; the levels and the keys (a cell's neighbours and
  the other octants apart; the hash's avalanche; the update's block within
  the budget); the views that take it and those that do not.
- **Ambient occlusion** (`test_ao.c`), against a ground truth cast by 576
  stratified rays a pixel: a box on the ground occluded within a quarter of
  how much the truth is, with no more than a hundredth of light on average
  where the truth is open; the ground alone open out to its horizon and at
  the image's edges; the diffuse light's bounce, measured from two albedos
  with and without the occlusion, against `nph_render_ref_ao_bounce`; a sun
  alone lighting the frame as without it, bit for bit, and the
  environment's light nowhere brightened; the views that make it (lit and
  indirect with an environment, the occlusion debug view) and those that
  do not; radii that are negative or not finite refused.

The DFG comparison found a real bug: in single precision, near-mirror GGX
samples produced `cos θ > 1`, so `sin θ` was NaN and 29 % of the samples
were silently dropped.

The whole-image tests are in `tests/golden` ([docs/golden.md](golden.md),
Phase 7's exit criterion). Nine scenes are compared with lavapipe
baselines by our own LDR-FLIP, with thresholds calibrated against the
RTX 3060 Ti. `render_bench` times the primitives and the golden
comparisons.

## Samples

`pbr_viewer` renders `samples/pbr_viewer/content/pbr_scene.gltf`, which is
cooked at build time. The scene contains tiles with normal,
occlusion-roughness-metallic, and colour maps (plus a mirrored copy), an
emissive panel, a masked double-sided grate, a glass pane, an unlit marker,
a gold roughness ramp, and a sun, a lamp, and a spotlight. It lights them
with a procedural dusk sky, orbits the camera, draws light gizmos, and logs
the profiler. `--capture`, `--profile`, `--tonemapper`, `--view`,
`--pipeline`, and `--ev100` serve CI and debugging; P switches between the
pipelines. `scene_demo` draws its thousands of asteroids on the production
path and logs what the GPU culled.

## Limitations (future work)

- Levels of detail (ADR-0060): the shadow rays' and radiance cache's
  bottom levels and the reference renderer draw level 0; the error is
  geometric only (normals and texture coordinates inside a coarse level are
  not measured); the levels are not streamed.

- Only the production path's sun has a virtual shadow map; other lights'
  shadows are ray traced, and only where the device has ray queries. Many
  shadow-casting local lights do not fit the budget as rays (1.35 ms for
  1,024 lamps): their shadows, and screen-space contact shadows, come in
  Phase 14 with many-light sampling (ADR-0033, ADR-0041).
- The sun's map: a moving caster redraws its pages with every caster in
  them (static and dynamic pages are not kept apart); filtering is clamped
  half a texel inside a page at its edges; a level's texels double at its
  distance boundary, with no blend between levels; and its lookup costs
  the resolve 0.27 ms, most of it registers.
- Shadow rays do not handle the shadow terminator: on a coarse mesh, an
  interpolated normal can face a light that the triangle's plane does not,
  and the triangle's neighbours then shadow it (Hanika, "Hacking the
  Shadow Terminator", *Ray Tracing Gems II*, 2021).
- The production path's top level spans every instance slot up to the
  pool's high-water mark, live or not (the inactive ones last), and its
  rebuild costs accordingly.
- Temporal anti-aliasing costs 0.75 ms at 4K, past its §10.4 line
  (ADR-0034), and bloom adds 0.36 ms (ADR-0035); blended surfaces are
  resolved with the motion of the opaque surface behind them, and there is
  no sharpening. No MSAA. The HDR target's dither is the same every frame.
- Depth of field adds 0.97 to 1.54 ms at 4K with a lens wide open; its
  circles are cut at 32 pixels; what a
  sharp surface in front hides cannot show through its blur; bokeh is a
  uniform disc; blended surfaces take the depth of the opaque surface
  behind them (ADR-0037).
- Global illumination adds 0.20 to 0.41 ms at 4K, which with the ambient
  occlusion's 0.38 ms puts the sky-lit field 0.05 ms past the indirect
  light's §10.4 line, and its rays have a frame in which anything moves
  rebuild the top level (0.36 ms; ADR-0039). Point and spot lights do not
  bounce; devices without ray queries have no cache (no SDF or software
  tracing); blended surfaces and the reference renderer take the
  environment's irradiance; normal maps do not modulate the cache's
  irradiance; a changed scene shows over about 128 samples a cell;
  NVIDIA's patent on hashed path-space filtering is to be reviewed before
  a release (ADR-0039).
- Ambient occlusion adds 0.38 ms at 4K (ADR-0038), most of the indirect
  light's §10.4 line. Screen space cannot see how thick an occluder is
  (behind a silhouette it occludes as if it went on; the paper's thickness
  heuristic is not used); the environment's specular light is occluded by
  the occlusion's amount, not by a cone around the bent normal; the
  occlusion is not accumulated on its own (the temporal resolve averages
  its rotations in the image); blended surfaces and the reference
  renderer have none.
- Motion blur adds 0.51 to 0.60 ms at 4K with typical motions (ADR-0036).
  Blended surfaces blur with the motion of the opaque surface behind them;
  motion is linear across the shutter; a blur reaches at most 32 pixels
  either way; its sampling noise is left to change every frame; the first
  frame after a cut does not blur; the reference renderer has none.
- Bloom has no lens effects (dirt, flares, streaks), and the exposure is
  global: no local exposure, no centre-weighted metering. With bloom off,
  an automatic exposure still runs the chain's first downsample for its
  histogram.
- The reference renderer evaluates every light for every pixel (it is the
  ground truth).
- The resolve is one shader for every material: tile classification
  returns with a second shading model (ADR-0031).
- The light passes run on the graphics queue; they read nothing of the
  frame's raster and could overlap it on the async compute queue.
- The first depth pyramid reads the whole visibility buffer (0.145 ms at
  4K); the second rebuilds its coarser mips in full.
- The software raster gives a cluster of a dozen triangles a whole
  32-thread workgroup.
- `KHR_texture_transform`, `TEXCOORD_1`, skinning, morph targets, and
  the PBR extension materials (clearcoat, transmission, ...) are not
  rendered (the cooker skips them, docs/cook.md).
- Draw records are built on the main thread: at reference-scene sizes this
  costs microseconds, and the production path moves it to the GPU.

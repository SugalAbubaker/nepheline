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
  (`NphRenderFrameStats.shadow_builds`), preferring trace speed.
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
    changed, after a frame that traced nothing, or when the camera has
    moved more than 16 m from the anchor, which then moves to the camera.
    A camera moving through a still scene costs no build.
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
buffer), and the scratch 9.8 MB. The bottom levels are not compacted.

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
| `vis hzb 0`, `vis hzb 1` | the depth pyramid's mip-0 passes **request** pages: each pixel's point rebuilt in light space from its depth (`light_from_clip`), and the pages its shading may read set in the requests bitmask |
| `vsm requests (blended)` | with blended surfaces, which the visibility buffer does not hold: the blended draws with no attachments, each fragment in front of the opaque depth (the visibility buffer's, tested in the shader) setting its pages (`vsm_request.frag`) |
| `vsm update` | every entry against this frame's windows: an entry whose page the window left gives its physical page up; a requested entry with a page keeps it (and is drawn again if dirty), one without asks for a page |
| `vsm collect`, `vsm alloc` | the free physical pages, then the evictable ones (owned, not requested: those unused for 60 frames before the recent ones), handed to the entries that asked |
| `vsm cull instances`, `vsm cull meshlets` | for each level with pages to draw (an indirect dispatch: none in a frame that keeps every page), the instances whose bounding spheres reach such a page, then their meshlets: a draw per (meshlet, page), scanning only the rows that hold a page to draw |
| `vsm fixup` | where the draws did not all fit, the pages drawn are kept dirty, to be drawn again |
| `vsm raster` | the pages to draw cleared (a quad each), then the casters drawn into them, both faces, depth clamped, four clip distances confining each draw to its page; mesh shaders, or indexed indirect draws |
| `vsm statistics` | the counters, read back two frames later |

**Requests.** A pixel asks for the level whose texels match its footprint
(the texel at most twice the footprint) or, where that level's window
does not hold its point with a margin, the next that does (a narrow view
from afar), and there the page that holds the point and any other within
2 texels of it (the shading's offset along the normal, and slack): at
most 2 × 2 pages. Mostly every pixel of a subgroup needs the same page,
which one invocation sets; otherwise, round after round, the lowest page
any invocation still needs is set by one of those that need it.

**Caching.** A drawn page stays drawn. A caster's change dirties exactly
the pages under its bounding sphere, old and new, since a shadow map's
texel holds the nearest caster along the light; a dirty page is drawn
again (with every caster in it) when next requested. The sun turning, the
anchor moving, or a production frame without the map drops every page.

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

**Overflow.** What does not fit the pool, `max_vsm_meshlets`, or
`max_vsm_draws` is dropped, flagged in `NphRenderFrameStats.vsm_overflow`,
and logged once: pixels whose pages did not fit are lit, a page whose
draws did not all fit is drawn again the next frame. Each overflow at the
current **resolution bias** raises it (each step halves every level's
resolution, up to 3), so the pages fit within a few frames; 120 frames in
a row whose pages would fit a step finer within three quarters of the
pool lower it again (`vsm_lod_bias`).

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

### A frame

| Pass | What it does |
|---|---|
| `vis clear` | clears the visibility buffer and the counters |
| `vis scatter` | applies the frame's instance updates |
| `vis light updates`, `vis lights`, `vis light bins` | the light grid's lights and distance bins (below), from the light table; nothing here reads the visibility buffer |
| `vis cull instances 0` | per instance: frustum, then occlusion against **last** frame's depth pyramid with last frame's transform and camera; survivors' meshlets go out as (instance, meshlet) work items, the occluded instances wait for phase 1 |
| `vis cull meshlets 0` | per work item: frustum, normal cone (similarity transforms only), occlusion; survivors are appended to the cluster list and to a raster list: opaque, masked, or software |
| `vis raster 0`, `vis sw raster 0` | the hardware raster of the opaque and masked clusters, then the compute raster of the small ones, into the visibility buffer |
| `vsm clear`, `vsm invalidate` | while the sun has its virtual shadow map ("Shadows"): its counters and bitmasks cleared, and the pages this frame's instance updates touch marked dirty, before the scatter replaces their records |
| `vis hzb 0` | the depth pyramid of what phase 0 drew (and the sun's map's page requests, the ambient occlusion's depths, and the light tiles' depth bounds) |
| `vis cull instances 1`, `vis cull meshlets 1` | the deferred instances against the new pyramid; then the meshlets phase 0 deferred and those of the instances now visible |
| `vis raster 1`, `vis sw raster 1`, `vis hzb 1` | what phase 1 found visible; the pyramid the next frame tests against, rebuilt only where phase 1 drew (and the map's requests, the occlusion's depths and the light tiles' bounds there) |
| `vis light tiles` | the light grid's tile masks, culled against the tiles' depth bounds (below) |
| `vsm requests (blended)` | with the map and blended surfaces: the blended surfaces' page requests, against the visibility buffer's depth |
| `vsm update` to `vsm statistics` | the sun's map: page tables, allocation, the casters of the pages to draw, and those pages drawn ("Shadows") |
| `ao depth`, `ao`, `ao denoise` | on a view with ambient occlusion: the depths' coarser mips, each pixel's slice, and the denoise (below) |
| `rt instances`, `rt accel` | while a light casts shadows that the sun's map does not take, or the view has global illumination: the top level's records from the GPU scene and its build, when it must be rebuilt, and new meshes' bottom levels ("Shadows") |
| `gi clear`, `gi update`, `gi resolve` | on a view with global illumination: the radiance cache's slots zeroed (its first frame), its rays, and its cells' blend (below) |
| `vis resolve` | shades every pixel from its cluster and triangle (the sun's map read once per pixel; shadow rays for the other lights that cast shadows; the pixel's ambient occlusion; the radiance cache's cell looked up) |
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
field's 0.154 against 0.159. Bistro's pass is the shadow map's page
requests (about 0.14 ms) as much as the read.

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

Measured and not kept: the light terms rearranged exactly (the range
test's vector reused, one reciprocal square root for the direction and
the falloff, N·H and V·H from |v + l|² = 2 + 2 v·l, the distribution and
the visibility over one division, Schlick's power as multiplies: four
transcendentals a light instead of nine), with N·L tested before the rest
of the record loads, and with two candidates' positions loaded together.
The 1,024 lamps' resolve took 1.37 ms against 1.42, but the compiler then
allocated 64 registers and spilled 5,888 bytes a workgroup, and Bistro's
hedge view took 3.18 ms against 3.08 (the street the same).

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
in the 4K field. The pass is not waiting on that chain.

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
  alone; a block of the sky alone is written open at once. The occlusion
  goes out as a half float.
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
take one direction whole (0.87 ms against 0.84).

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
  at 4K): the surface point rebuilt as the resolve rebuilds it, its cell
  claimed, one cosine-distributed ray traced. A miss adds the
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
(2^24, indirect path only); `max_lights` is at most 4,096. A list that fills drops work and sets a bit in
`NphRenderFrameStats.vis_overflow`, logged once. The sun's map has
`vsm_pages` (2,048), `max_vsm_meshlets` (2^20), and `max_vsm_draws`
(2^20), whose overflow (`vsm_overflow`) raises its resolution bias
("Shadows"); its statistics are `vsm` and `vsm_lod_bias` (this frame) and
the pages requested, drawn, held, and evicted, and the caster draws, read
back as the others. The radiance cache has `gi_cells` (2^18) slots, a
bucket of which, when full, drops the samples of the cells it cannot
take, and `gi_rays` (65,536) a frame. The frame statistics
carry the GPU's counters (clusters and triangles drawn, clusters drawn in
software, instances and meshlets culled by each test, lights in view),
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
overlapped enough to pay for them. The profiler logs a table and writes
JSON for the benchmarks.

CPU time: `nph_render_frame` is the profiler zone `render.frame`
([profile.md](profile.md)); the passes it declares are recorded under the
render graph's zones.

## Testing

`render_tests` (138 tests, registered with CTest as the four shards
`render.0` to `render.3`, docs/ntest.md) run on the RTX 3060 Ti locally
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
  the rays it replaces (98 % of pixels within two levels); and the scene
  10,000 km from the origin.
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

- Only the production path's sun has a virtual shadow map; other lights'
  shadows are ray traced, and only where the device has ray queries. Many
  shadow-casting local lights do not fit the budget as rays (1.35 ms for
  1,024 lamps): their shadows are stage 6's next part. Screen-space
  contact shadows are deferred (ADR-0033).
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

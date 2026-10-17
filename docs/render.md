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
- The GPU-driven **production path** (stages 3–4,
  [ADR-0029](adr/0029-gpu-driven-visibility-buffer.md); "The production
  path" below) culls, rasterises into a visibility buffer, and shades on
  the GPU. Its CPU cost does not depend on the object count. Its images
  are tested against the reference's.

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
| mesh | GPU, immutable | the cooked streams as they are: unorm16 positions in the bounds, octahedral normals, snorm8 tangents, half UVs, RGBA8 colours; up to 256 submeshes |
| material | CPU record | metallic-roughness glTF material with five texture bindings, each with its own wrap and filter |
| instance | CPU record | a mesh, a material per material slot, a 64-bit transform, hidden flag |
| light | CPU record | directional, point, or spot |

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
| `forward` | graphics | lighting tables, SH | pre-exposed RGBA16F colour, D32 depth |
| `tonemap` | graphics | HDR colour (sampled), depth (read-only attachment) | the caller's target |

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
  Normal maps follow the MikkTSpace convention of glTF (the interpolated,
  unnormalised frame; bitangent `cross(n, t) · w`). Two-channel maps
  (BC5, RG8) rebuild z. Meshes without tangents get a frame from
  screen-space derivatives (Schüler 2006).
- **Occlusion** applies to indirect light only (glTF), and also drives
  specular occlusion (Lagarde & de Rousiers 2014).
- **Alpha:** MASK discards below the cutoff in its own shader variant (so
  the opaque variant keeps early depth testing). BLEND composites "over"
  without writing depth. Double-sided materials flip the whole tangent
  frame on back faces.
- **Unlit** materials (`KHR_materials_unlit`) write their colour as
  authored, outside exposure.
- **Debug views:** base colour, shading normal, roughness, metallic,
  occlusion, direct light only, and indirect light only; on the
  production path also clusters, triangles, and instances (a hashed
  colour each).

`shaders/render/surface.glsl` holds this model; the forward pass and the
production path's resolve both include it, so the two renderers cannot
drift apart. The resolve has no rasteriser derivatives, so it supplies
its own (see below).

## The production path

`view.pipeline = NPH_RENDER_PIPELINE_PRODUCTION` draws the frame with the
GPU-driven visibility-buffer renderer (Haar & Aaltonen, "GPU-Driven
Rendering Pipelines", SIGGRAPH 2015; Burns & Hunt, "The Visibility
Buffer", JCGT 2013; Karis et al., "Nanite: A Deep Dive", SIGGRAPH 2021).
The CPU writes the view, the records of instances that changed, and the
pass declarations, a few microseconds whatever the object count
(`render_perf`, docs/benchmarks.md); the GPU culls, rasterises, and
shades. Blended surfaces stay on the forward pass, drawn over the result.

It needs 64-bit image atomics, 64-bit integers in shaders, subgroup ballot
and arithmetic, and the geometry-shader feature (which lets a fragment
shader read `gl_PrimitiveID`). `nph_renderer_production_supported` says
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

Creating, moving, hiding, or destroying an instance marks its slot; each
frame writes at most `max_instance_updates` update records into the
frame's upload buffer, and `vis scatter` copies them into the tables (the
rest wait a frame, logged). A destroyed instance's material slots return
`NPH_GPU_FRAMES_IN_FLIGHT` frames later, after the frames that may read
them; the range allocator has room for a whole population being replaced
between two frames.

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
| `vis cull instances 0` | per instance: frustum, then occlusion against **last** frame's depth pyramid with last frame's transform and camera; survivors' meshlets go out as (instance, meshlet) work items, the occluded instances wait for phase 1 |
| `vis cull meshlets 0` | per work item: frustum, normal cone (similarity transforms only), occlusion; survivors are appended to the cluster list and to a raster list: opaque, masked, or software |
| `vis raster 0`, `vis sw raster 0` | the hardware raster of the opaque and masked clusters, then the compute raster of the small ones, into the visibility buffer |
| `vis hzb 0` | the depth pyramid of what phase 0 drew |
| `vis cull instances 1`, `vis cull meshlets 1` | the deferred instances against the new pyramid; then the meshlets phase 0 deferred and those of the instances now visible |
| `vis raster 1`, `vis sw raster 1`, `vis hzb 1` | what phase 1 found visible; the pyramid the next frame tests against, rebuilt only where phase 1 drew |
| `vis resolve` | shades every pixel from its cluster and triangle |
| `vis statistics` | copies the counters for the frame's statistics |
| `vis depth`, `transparent` | the depth the visibility buffer holds, then the blended surfaces, forward |
| `tonemap` | as the reference's |

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
under reverse-Z with no depth attachment, so the hardware and software
rasters write the same buffer the same way.

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
reaches the edge. An occlusion test projects the bounding sphere to a
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
(0.066 ms in the 4K field). A cluster it cannot bound, or one covering
more than 64 tiles, asks for a full rebuild. A test compares the result
with a full rebuild's, texel for texel, after a wall moves off what it
hid.

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
reference's (100 % of pixels within 3 levels in the tests).

### Capacities and statistics

The production path's lists have fixed capacities in `NphRendererDesc`:
`max_visible_clusters` (524,288), `max_cull_meshlets` (2^20, phase 0's
meshlet work items), `max_deferred` (262,144: deferred instances, and
phase 1's meshlets), `max_instance_updates` (16,384),
`max_material_slots` (4 × `max_instances`), and `meshlet_index_pool`
(2^24, indirect path only). A list that fills drops work and sets a bit in
`NphRenderFrameStats.vis_overflow`, logged once. The frame statistics
carry the GPU's counters (clusters and triangles drawn, clusters drawn in
software, instances and meshlets culled by each test), read back from the
frame that completed
`NPH_GPU_FRAMES_IN_FLIGHT` frames earlier (`vis_frame`). On this path
`instances` is the live count and `culled` the hidden instances plus
blended ones outside the frustum; `draws` and `triangles` count only the
blended surfaces the CPU draws.

### Cost

`render_perf` on the RTX 3060 Ti (Profile; docs/benchmarks.md), the CPU
time of `nph_render_frame` and the GPU frame:

| Scene | Instances | CPU, reference | CPU, production | GPU, reference | GPU, production |
|---|---|---|---|---|---|
| grid, 512 × 512 | 1,024 | 0.20 ms | 0.005 ms | 0.05 ms | 0.16 ms |
| grid, 512 × 512 | 10,240 | 1.26 ms | 0.005 ms | 0.17 ms | 0.21 ms |
| grid, 512 × 512 | 102,400 | 13.4 ms | 0.005 ms | 3.1 ms | 0.64 ms |
| field, 3840 × 2160 | 16,385 | 2.86 ms | 0.006 ms | 2.69 ms | 1.89 ms |

In the 4K field the surviving clusters hold 5.3 million triangles (the
reference draws 14.6 million after its frustum test; occlusion removed
53,257 clusters, and 55,701 of the 62,234 drawn went to the software
raster), and the passes take: clear 0.08 ms, culling with its argument
passes 0.10 ms, raster 0.50 ms (0.22 in hardware, 0.28 in software), the
two pyramids 0.21 ms, resolve 0.87 ms, tonemap 0.12 ms. Against CLAUDE.md
§10.4, the raster (0.65 ms budgeted) and the shading fit; culling with
the depth pyramid takes 0.31 ms against 0.25 ms, most of it the first
pyramid's read of the visibility buffer. ADR-0029 and ADR-0030 record the
numbers and what comes next.

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

`tonemap.frag` maps pre-exposed colour to the display:

- **ACES:** Stephen Hill's fit of the ACES 1.0 RRT and sRGB ODT
  (BakingLab, 2016), with the input and output matrices.
- **AgX:** Troy Sobotka's AgX with the base contrast look: Blender's
  inset/outset matrices and Benjamin Wrensch's sixth-order polynomial fit
  of the sigmoid (2023).
- **None:** a clamp.

On UNORM targets the shader applies the exact sRGB curve; sRGB targets are
encoded by the hardware. Tests compare both against the C reference, texel
by texel, within one 8-bit step.

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
smaller, and repeated reads are not counted. The profiler logs a table and
writes JSON for the benchmarks.

CPU time: `nph_render_frame` is the profiler zone `render.frame`
([profile.md](profile.md)); the passes it declares are recorded under the
render graph's zones.

## Testing

`render_tests` (68 tests) run on the RTX 3060 Ti locally and on the
self-hosted runner, and on lavapipe in CI, under the validation and
synchronisation-validation gate:

- **CPU:** GGX normalisation, Smith symmetry and bounds, DFG energy and
  limits, tonemap ranges and the ACES value at middle grey (worked by
  hand), sRGB round trips, cube-face layout, texel solid angles tiling the
  sphere, SH of a constant environment (π·L), and every primitive's
  winding, tangent handedness, and quantisation.
- **GPU against CPU:** the tonemap curves on UNORM and sRGB targets; the
  DFG table; the SH projection; constant environments through every filter
  (sky, equirectangular, cube); BRDF radiance under directional, point, and
  spot lights; image-based lighting energy conservation; textures and
  debug views; normal maps on mirrored instances, three- and two-channel;
  sidedness, alpha modes, blend order, culling; camera-relative rendering;
  debug lines; and the profiler's windows and declared bytes.
- **The production path** (`test_vis.c`): its images against the
  reference's (plain, with every alpha mode and side, and 10,000 km from
  the origin); culling switched off changes nothing; the frustum culls
  what is off screen; mesh shaders and indirect draws give the same image,
  and so do the software and hardware rasters, with no cracks between
  their triangles; instances move, hide, and go, and a whole population is
  replaced between frames; the debug views; cooked and built meshlets draw
  the same; the culling math against its C twins; the depth pyramid read
  back, and the one after phase 1 against a full rebuild.

The DFG comparison found a real bug: in single precision, near-mirror GGX
samples produced `cos θ > 1`, so `sin θ` was NaN and 29 % of the samples
were silently dropped.

The whole-image tests are in `tests/golden` ([docs/golden.md](golden.md),
Phase 7's exit criterion). Eight scenes are compared with lavapipe
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

- No shadows: virtual shadow maps are §8.6 stage 6 (Phase 10).
- No MSAA or TAA: TAA is stage 8 (Phase 10).
- Every light is evaluated for every pixel: clustering is stage 5.
- The resolve is one shader for every material until stage 5 classifies
  tiles.
- The first depth pyramid reads the whole visibility buffer (0.145 ms at
  4K); the second rebuilds its coarser mips in full.
- The software raster gives a cluster of a dozen triangles a whole
  32-thread workgroup.
- `KHR_texture_transform`, `TEXCOORD_1`, skinning, morph targets, and
  the PBR extension materials (clearcoat, transmission, ...) are not
  rendered (the cooker skips them, docs/cook.md).
- Draw records are built on the main thread: at reference-scene sizes this
  costs microseconds, and the production path moves it to the GPU.

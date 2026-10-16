# Render module (`engine/render`)

The forward PBR **reference renderer** (CLAUDE.md §8.6 stages 1–2), plus the
pieces every later renderer reuses: image-based lighting, tonemapping, the
debug-draw API, the GPU profiler, and procedural primitives. Decisions are
recorded in [ADR-0018](adr/0018-reference-renderer.md).

The reference renderer is the **ground truth**, not the production path.
It draws each visible submesh with its own draw call and evaluates every
light at every pixel. Its CPU cost grows with the object count and its GPU
cost with lights × pixels. The GPU-driven visibility-buffer renderer of
§8.6 stages 3–8 (Phase 10) will be compared against its images, so it
favours exactness over speed. Every formula is cited in the shader that
implements it, and has a CPU twin that the tests check it against.

| Header | What it holds |
|---|---|
| `renderer.h` | the renderer: textures, meshes, materials, instances, lights, the environment, frames |
| `debug_draw.h` | immediate-mode debug lines (depth-tested and overlay) |
| `gpu_profiler.h` | per-pass GPU times and declared bytes over a rolling window |
| `primitives.h` | cube, sphere, plane, cylinder in the cooked vertex streams |
| `reference.h` | CPU reference implementations of the shader math |

Shaders live in `shaders/render/`. The records they share with C are in
`shaders/shared/nph_render.h`.

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
  occlusion, direct light only, and indirect light only.

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
are relative to a 64-bit debug origin. Each of the two lists (depth-tested,
overlay) holds `debug_lines` lines; extra lines are dropped and counted.

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

`render_tests` (52 tests) run on the RTX 3060 Ti locally and on lavapipe
in CI, under the validation and synchronisation-validation gate:

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
the profiler. `--capture`, `--profile`, `--tonemapper`, `--view`, and
`--ev100` serve CI and debugging.

## Limitations (future work)

- No shadows: virtual shadow maps are §8.6 stage 6 (Phase 10).
- No MSAA or TAA: TAA is stage 8 (Phase 10).
- Every light is evaluated for every pixel: clustering is stage 5.
- One draw call per submesh, CPU culling: the GPU-driven path is stage 3.
- `KHR_texture_transform`, `TEXCOORD_1`, skinning, morph targets, and
  the PBR extension materials (clearcoat, transmission, ...) are not
  rendered (the cooker skips them, docs/cook.md).
- Draw records are built on the main thread: at reference-scene sizes this
  costs microseconds, and the production path moves it to the GPU.

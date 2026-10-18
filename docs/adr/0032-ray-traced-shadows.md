# 0032. Ray-traced shadows: the ground truth for every shadow technique, and the production path's first

- **Date:** 2026-10-17
- **Status:** Accepted

## Context

CLAUDE.md §8.6 stage 6 asks for virtual shadow maps with page caching, so
that only pages invalidated by movement are redrawn, plus screen-space
contact shadows for fine detail. Until now neither renderer drew shadows
at all.

Two things are needed before a shadow-map technique can be built and
trusted:

- **A ground truth.** The reference renderer is what the production path's
  images are tested against (ADR-0018, ADR-0029). Shadow maps are
  approximations: resolution, depth bias, and filtering each leave their
  mark, and a cached virtual shadow map adds invalidation to the list. A
  shadow-map reference would test them against another approximation.
- **A way to see the error.** The golden-image tests (ADR-0019) need a
  baseline that is right by construction, on lavapipe in CI and on the RTX
  3060 Ti.

The reference hardware has second-generation RT cores, and CLAUDE.md §3
lists `VK_KHR_ray_query` and `VK_KHR_acceleration_structure` as optional
features for stage 10. Lavapipe implements ray queries, so ray tracing can
run in CI as well.

Measurements are `render_perf` on the RTX 3060 Ti, Profile build, at
3840 × 2160 in the 4K field (16,385 instances, 5.3 million triangles drawn
by the production path), unless stated otherwise.

## Decision

1. **Acceleration structures in the RHI** (`gpu/accel.h`, recorded with
   the commit that added them). Shaders reach a top level by its device
   address (`accelerationStructureEXT(address)`), so the bindless layout is
   unchanged. The render graph knows the build and trace usages.
2. **Each light can cast shadows** (`NphRenderLightDesc.shadows`; the
   runtime's `Light.shadows`). A shaded point that a shadow-casting light
   reaches (inside its range and cone, facing it) traces one ray toward
   it:
   - a directional light's ray is unbounded; a point or spot light's ends
     at the light;
   - the first hit occludes and every triangle is opaque, so a point is
     lit or not, and the shadows are hard (punctual lights have no area);
   - the ray leaves from the side of the geometric normal that faces the
     light, its origin pushed off by 256 units in the last place of each
     coordinate (Wächter & Binder, "A Fast and Robust Method for Avoiding
     Self-Intersection", *Ray Tracing Gems*, 2019, chapter 6), so it never
     meets the triangle it starts on at any distance.

   The test lives in `surface.glsl`'s light loop (`NPH_SURFACE_SHADOW`).
   The reference's forward pass, the production path's resolve, and its
   blended surfaces therefore trace the very same rays. Each has a variant
   compiled with ray queries (`*_rt`), taken in the frames where a light
   casts shadows, so the other frames pay nothing.
3. **One bottom level per mesh**, over the cooked quantised positions as
   they are (R16G16B16A16_UNORM, which the RTX 3060 Ti and lavapipe build
   from; a float copy of the same values elsewhere). Each is built once,
   in the first frame that traces after the mesh is created, preferring
   trace speed.
4. **One top level over every instance slot.** Each instance record folds
   the mesh's dequantisation into its transform. Hidden and free slots get
   a zero mask and no structure, so no ray meets them. The top level has a
   space of its own: the world less an anchor near the camera, so 32-bit
   coordinates hold anywhere in a 64-bit world (§8.2). Shaders move a view
   point into it by adding `NphRenderViewData.tlas_offset`, the camera's
   position less the anchor.
   - **The production path** writes the records on the GPU from its GPU
     scene (`rt instances`), so its CPU cost does not depend on the
     instance count. It rebuilds the top level only when its instances
     changed this frame, after a frame that traced nothing, or when the
     camera has moved more than 16 m from the anchor (which then moves to
     the camera). A camera moving through a still scene costs no build.
     The 16 m bound keeps the ray offset near the camera at 256 ulp(16 m),
     half a millimetre.
   - **The reference** writes the records on the CPU, as it does its
     draws, and rebuilds every frame with the anchor at the camera.
5. **No frame allocates.** One scratch buffer serves every build. It is
   sized at creation for the top level of `max_instances` and grown at
   mesh creation when a bottom level needs more. A frame's bottom-level
   builds fill it in batches, with a barrier between batches.
6. **Where the renderer has no ray tracing** (no ray queries, or
   `NphRendererDesc.disable_ray_tracing`), shadow-casting lights draw
   unshadowed, logged once. Stage 6's virtual shadow maps are the shadows
   such devices get. Ray-traced shadows stay as the ground truth they are
   tested against, and as a quality tier.

## Measurements

| Scene (16,385 instances) | GPU frame | Resolve | Top-level build | CPU |
|---|---|---|---|---|
| day field, no shadows | 1.941 ms | 0.901 ms | | 0.007 ms |
| day field, the sun's shadows, nothing moving | 2.352 ms | 1.317 ms | none (kept) | 0.007 ms |
| the same, one sphere moving every frame | 2.685 ms | 1.303 ms | 0.319 ms + records 0.033 ms | 0.007 ms |
| night field, 1,024 lamps | 2.241 ms | 1.179 ms | | 0.007 ms |
| night field, 1,024 lamps and the moon casting shadows | 3.602 ms | 2.545 ms | none (kept) | 0.007 ms |
| reference renderer, day field, no shadows | 2.682 ms | (forward 2.576 ms) | | 2.91 ms |
| reference renderer, the sun's shadows | 3.893 ms | (forward 2.914 ms) | 0.872 ms (from host memory) | 3.38 ms |

- **The sun's shadows** add 0.41 ms to a still scene, all of it in the
  resolve's rays. That is just inside §10.4's shadow line (0.40 ms). A
  scene with motion adds the rebuild on top: 0.62 ms in all, over the line
  by 0.22 ms.
- **The rebuild walks every slot up to the instance pool's high-water
  mark.** `render_perf` ran its 102,400-instance grid before the field, so
  the field's top level spans 102,401 slots: 0.319 ms. Run alone, the same
  scene rebuilds 16,386 slots in 0.222 ms, and its records take 0.010 ms.
- **Before the anchor**, the top level was camera relative and rebuilt
  every frame. The still day field took 2.839 ms then (rebuild 0.319 ms),
  against 2.352 ms now.
- **The resolve with ray queries** compiles to 72 registers and 6.9 KB of
  shared memory per workgroup (the driver's traversal stack), against 64
  registers and 0.8 KB without. That is the reason for the variant.
- **The 1,024 lamps' shadows** (every lit point tracing a ray to each of
  the roughly fifteen lamps that reach it) add 1.37 ms. Ray-traced shadows
  for many local lights do not fit the budget as they are.
- **Memory** for `render_perf`'s 119,041 instance slots: a 20.7 MB top
  level, 7.6 MB of records (and, for the reference, 7.6 MB more in each
  frame's upload buffer), and a 9.8 MB scratch buffer, besides each mesh's
  bottom level. The bottom levels are not compacted.
- **Agreement.** The production path's shadows match the reference's: the
  golden `shadows` scene's production render scores a FLIP mean of 0.013
  and a p99 of 0.036 against the reference's on the RTX 3060 Ti, the same
  contour noise of the compact HDR target as the other scenes, with no
  error at shadow edges.

## Alternatives considered

- **Shadow maps in the reference renderer:** an approximation as the
  ground truth (resolution, bias, filtering), against which a virtual
  shadow map's own errors could not be told apart.
- **Ray-tracing pipelines** (`VK_KHR_ray_tracing_pipeline`): shader binding
  tables and a separate dispatch per light, where a shadow ray needs no
  hit shader at all. Inline ray queries trace from the resolve that
  already has the point, in one pass.
- **A screen-space shadow mask per light**, traced in its own pass so that
  the resolve keeps its 64 registers: one image of reads and writes per
  light, and positions and normals rebuilt from the visibility buffer a
  second time. This is worth measuring for the sun alone if ray-traced
  shadows become a production tier on their own.
- **Refitting the top level** (an update build) instead of rebuilding it:
  cheaper per frame, but its quality decays as instances move apart. With
  the anchor, a still scene builds nothing at all, which no refit beats.
- **A camera-relative top level, rebuilt every frame:** what the first
  version did, measured above at 0.32 ms a frame whatever moved.

## Consequences

- The reference renderer has exact hard shadows, and so does the
  production path wherever the device has ray queries. Stage 6's virtual
  shadow maps are tested against them, pixel for pixel, in the shadow
  tests and the golden scene.
- Lights cast no shadows unless asked to (`shadows = 0` by default, in
  text scenes as well). glTF's `KHR_lights_punctual` has no such property,
  so imported lights cast none until edited.
- New statistics: `NphRenderFrameStats.shadow_rays`, `shadow_builds`, and
  `shadow_rebuild`. New render-graph passes: `rt instances` (production
  path) and `rt accel`, which build into imported buffers outside the
  graph's aliasing.
- The renderer's contract now says what was already true: no resource may
  be created or destroyed between `nph_render_frame` and the graph's
  execution, since the passes record from them.
- Open: the shadow terminator on coarse meshes, where the interpolated
  normal faces a light that the triangle's plane does not (Hanika,
  "Hacking the Shadow Terminator", *Ray Tracing Gems II*, 2021);
  compaction of the bottom levels; a top level over live slots only;
  local lights' shadows within budget (cached, or a few lights at a time),
  which is stage 6's next part.

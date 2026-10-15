# 0018. The reference renderer: forward PBR in physical units, GPU-computed IBL, pre-exposed HDR

- **Date:** 2026-10-15
- **Status:** Accepted

## Context

CLAUDE.md §8.6 builds the renderer in stages. Stage 1 is the bring-up (mesh,
depth, textures, camera). Stage 2 is a "forward PBR reference renderer, kept permanently as the ground truth for
golden-image comparisons. It is not the production path": a
metallic-roughness GGX BRDF (Karis 2013; Burley 2012), image-based
lighting with the split-sum approximation, and an HDR target with ACES-fit
and AgX tonemapping. Phase 7 also asks for a debug-draw API and a GPU
profiler per render-graph pass, and ends when "golden-image tests pass for
the PBR test scenes" (§12).

Constraints: the module order puts `render` left of `asset` (§4); positions
are 64-bit on the CPU and camera-relative on the GPU (§8.2); per-frame work
allocates nothing (§10.5); every PR states its cost in milliseconds and
DRAM bytes (§10.7); validation and synchronisation validation stay silent
(§1.3). The renderer will be the reference that the visibility-buffer
renderer of Phase 10 is compared against, so its correctness matters more
than its speed. Its images must also be reproducible across GPUs (the
RTX 3060 Ti locally, lavapipe in CI).

## Decision

1. **Forward shading, one draw call per visible submesh, CPU frustum
   culling, every light evaluated for every pixel.** This is the simplest
   renderer whose output is unambiguous. Clustering, GPU culling, and
   indirect draws belong to the production path and would make the
   reference depend on the machinery it is meant to check.
2. **Physical units throughout.** Lights in lux and candela (glTF's
   `KHR_lights_punctual`). Emissive and environment radiance in cd/m².
   Exposure from EV100 (Lagarde & de Rousiers 2014). glTF content arrives
   with the right light units already. Emissive strength is read as cd/m²,
   with a scale in the bridge for content authored otherwise.
3. **A pre-exposed RGBA16F HDR target.** Exposure is applied in the
   forward pass, so sunlit specular peaks stay inside half-float range.
   R11G11B10 (§10.3.5's bandwidth format) loses the precision a reference
   needs and has no alpha for blending; the production path will use it.
4. **The full BRDF of the cited papers, with energy compensation.** GGX,
   height-correlated Smith visibility, Schlick Fresnel, and Lambert
   diffuse. Punctual light gets Kulla-Conty's compensation. Image-based
   lighting uses Fdez-Agüera's multiple-scattering split sum. Perceptual
   roughness is clamped at 0.045 (Filament). Without the compensation,
   rough metals lose up to 40 % of their energy, which a reference must
   not.
5. **Image-based lighting computed on the GPU in render-graph passes.**
   The environment cube comes from a procedural sky, an equirectangular
   texture, or a cube texture, followed by its box-filtered chain. The
   prefiltered specular levels use filtered importance sampling (Křivánek &
   Colbert 2008). Irradiance is band-2 SH (Ramamoorthi & Hanrahan 2001),
   reduced deterministically, and the DFG table is computed once. Nothing
   is baked offline, so any environment works at run time, and the graph
   derives every barrier.
6. **Tonemapping in its own pass into the caller's target**, with ACES
   (Hill's fit) and AgX (Blender's matrices, Wrensch's polynomial). Debug
   lines are drawn in the same pass, over the tonemapped image, against the
   scene depth as a read-only attachment.
7. **Records in per-frame upload buffers.** Materials, instances, and lights
   are CPU records copied to the frame-in-flight's buffer, so edits never
   race the GPU. Only textures and meshes are GPU-resident and immutable.
8. **The renderer takes the cooked streams unchanged.** Its vertex layout
   is the cooker's (unorm16 positions, octahedral normals, snorm8
   tangents, half UVs), so cooked meshes upload without conversion. A
   bridge in `samples/common` loads cooked scenes into it until the
   runtime module exists (Phase 9).
9. **Every shader formula has a C twin** (`render/reference.h`) that tests
   compare the GPU against. Pixel tests compare rendered images with the
   twins' predictions for scenes whose answer can be computed by hand. The
   golden tests then lock down whole scenes.
10. **The GPU profiler lives in render** and reports the render graph's
    *declared* bytes per pass (a new field of the gpu module's timings) as
    the DRAM figure. This is a footprint, not a measurement: Vulkan has no
    portable DRAM counters, and vendor tools are outside §3's library
    list.

## Alternatives considered

- **Deferred shading or a visibility buffer as the reference.** That is the
  production design. A reference that shares the production path's
  structure cannot catch that path's structural mistakes.
- **Relative ("artist") units with exposure 1.** These are simpler to
  author, but the radiance cache and virtual shadow maps of Phase 10 and
  the EV-driven auto-exposure all assume physical quantities. Converting
  later would change every golden image.
- **Baking IBL offline in the cooker.** This is faster at run time, but
  ties environments to cooked content and duplicates the prefiltering in
  C. Computing it in the graph costs under a millisecond, once, on the
  RTX.
- **An irradiance cube instead of SH.** A cube is more accurate for
  high-frequency environments, but costs another convolution pass and
  more memory. For diffuse lighting from skies, band 2 is within a few
  percent (Ramamoorthi & Hanrahan's 3 % bound).
- **Filament-style DFG with the multiple-scattering term in a third
  channel.** The two-channel table plus Fdez-Agüera's formulation gives
  the same compensation from the same data.
- **R11G11B10 for the HDR target.** See decision 3. It remains the plan for
  the production path.

## Consequences

- The reference is slow by design. It is compared against the production
  path at small resolutions and is not used for performance numbers. Its
  cost (0.14 ms per frame at 720p for the sample scene on the RTX) is
  logged by the profiler for completeness.
- Golden images depend on physical light levels and EV100. Changing the
  sun's illuminance changes images, so content uses glTF's units as they
  are.
- No shadows until virtual shadow maps (Phase 10). Golden scenes are lit
  so the missing shadows are not mistaken for regressions.
- The bridge in `samples/common` is shared by samples and tests. It moves
  to the runtime module in Phase 9.
- Writing the renderer's tests exposed two RHI bugs, now fixed and covered
  by tests:
  - `nph_gpu_device_wait_idle` and `nph_gpu_frame_begin` recycled the
    command pool of an open upload batch, so uploads queued between
    frames were lost. They now flush pending uploads first.
  - The asset module's `vfs.h` was not self-contained. Every public header
    is now compiled on its own by a generated translation unit
    (`nph_add_module`).

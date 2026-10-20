# 0040. Reference scenes: third-party content outside the repository, pinned by manifest

- **Date:** 2026-10-20
- **Status:** Accepted (Bistro measures 7.47 ms against the 4.17 ms frame:
  see Consequences)

## Context

CLAUDE.md §10.7 measures the targets on reference scenes that Phase 10
defines: dense urban, open landscape with foliage, and an interior with
many lights. The synthetic scenes of `render_perf` (grids, a field, lights,
shadows, motion, depth of field) isolate passes, but they have no
production content: no authored materials, no texture sets, no depth
complexity of a real street. The user chose third-party open content
(2026-10-19): Amazon Lumberyard Bistro for the city, a Poly Haven (CC0)
layout for the landscape, and Intel's Sponza with its candles for the
interior, the last for local use only and never redistributed.

The constraints:

- **Size and licences.** Bistro's source is 3.5 GB and its cooked pack
  1.35 GB, against the repository's 1 GB Git LFS quota. CC BY 4.0 asks
  for attribution wherever the content is redistributed, and Sponza may
  not be redistributed at all.
- **Reproducibility.** A measurement means nothing unless the scene is
  the same one every time.
- **The content as it ships.** Bistro's glTF conversion (NVIDIA's, as
  `zeux/niagara_bistro` edits it) uses DDS textures through
  `MSFT_texture_dds`, specular-glossiness materials
  (`KHR_materials_pbrSpecularGlossiness`), 6,006 nodes under a root scaled
  by 100, and tangents with a few non-finite values. The cooker read none
  of that.

## Decision

1. **Manifests in the repository, content outside it.** Each scene is a
   manifest in `benchmarks/scenes/` holding:
   - its source, as a git repository, a commit, and a document;
   - its licence and attribution;
   - its cooked pack and scene key;
   - its exposure (`ev100`);
   - its sky (`sky_intensity`, scaling the default daylight sky, which
     pairs with a 30,000 lux sun, to the scene's own sun);
   - its views, either the scene's camera or an eye and a target.

   `scripts/scenes.py fetch` checks out exactly the pinned commit (a
   shallow fetch of that commit) into a directory of the machine's
   choosing. `scripts/scenes.py cook` cooks it there incrementally. Nothing
   of the content enters the repository or its artifacts.
2. **`render_perf` measures them as the runtime runs.** With
   `--scenes=DIR` (or `NPH_SCENES`), each view becomes
   `<scene>_<view>_production`. It runs at 3840 × 2160 through the
   production path with the runtime's defaults: TAA, bloom 0.04, ambient
   occlusion at 0.5 m, and the radiance cache. It is lit by the scene's own
   lights and the manifest's sky, after the synthetic scenes. Missing packs
   skip with a message. `--capture-dir` writes the last frame, and
   `--debug-view` shows the material channels and normals.
   `bench.render_perf_reference` runs the path in CTest on pbr_viewer's
   scene at 320 × 180.
3. **The cooker reads the content as it ships** (docs/cook.md):
   - DDS sources in every BC format and the DX10 header;
   - `MSFT_texture_dds`;
   - specular-glossiness converted to metallic-roughness with the
     conversion the extension's specification publishes;
   - a primitive's non-finite tangents rebuilt, not refused.
4. **What the first scene found is fixed where it lives:**
   - **Uploads between frames stream through the staging ring.** An upload
     the ring cannot take flushes the pending ones, and the synchronous
     flush recycles its command buffers (docs/gpu.md "Uploads"). Bistro's
     load had taken 7,488 dedicated staging buffers.
   - **The cooked-scene bridge creates one render material per material
     asset.** The scene's table names one per instance slot: Bistro's
     2,909 slots name 254 materials, and the renderer holds 1,024.
   - **Normal maps build MikkTSpace's frame in its normalised variant**
     (docs/render.md "Textures"). The unnormalised frame assumed unit
     interpolants. Bistro's ×100 root stretched every tangent by 100 and
     shrank every normal by it, so its normal-mapped surfaces shaded in
     their own plane, in both pipelines.

## Alternatives considered

- **Content in Git LFS.** Over the quota several times, and a
  redistribution that CC BY 4.0 would need attributed and Sponza's terms
  forbid.
- **Scenes of our own** (a procedural city). Measurable, but not the
  production content the targets are about: no authored materials,
  textures, or depth complexity.
- **Converting Bistro offline** to metallic-roughness and PNG with another
  tool. A dependency outside the pipeline for a format the cooker should
  read anyway: these extensions are common in published glTF.
- **Normalising the frame in the resolve.** The resolve interpolates in
  object space and transforms once.
  - Transforming each vertex's normal and tangent first, then normalising
    and interpolating, is exactly what a vertex stage does. It spilled the
    resolve to shared memory: +0.25 ms at 4K, and every 4K scene went over
    budget.
  - Restoring each interpolant's object-space length after the transform
    is exact for similarity transforms. It costs 0.02 to 0.08 ms on every
    pixel, normal map or not.
  - The normalised variant in the normal-mapping branch costs nothing
    without normal maps and makes the two pipelines agree exactly. It
    differs from the unnormalised one only to second order in the frame's
    spread across a triangle. The synthetic scenes measured the same as
    before (within 0.03 ms), and the golden images pass.
- **A larger staging ring** would only move the threshold: any load
  larger than the ring hit the same path.

## Consequences

- A scene is reproducible from its manifest, but the content's hosting is
  external. If a repository disappears, the pinned commit can be restored
  from a machine's own checkout, kept locally and never redistributed.
- **Bistro does not meet the target.** On the RTX 3060 Ti (Profile, 240
  frames) it takes 7.47 ms mean and 8.17 ms at the 99th percentile,
  against 4.17 ms (docs/benchmarks.md):

  | Pass | Bistro | §10.4 line |
  |---|---|---|
  | Resolve: every pixel's material with up to five textures, and 69 of the 97 lights in view | 3.69 ms | 1.20 ms |
  | Raster, 1.41 M triangles in 22,585 clusters | 1.15 ms | 0.65 ms |
  | Hi-Z, two phases | 0.30 ms | inside the 0.25 ms culling line |
  | TAA | 0.86 ms | 0.40 ms, with post |
  | Ambient occlusion | 0.45 ms | 0.60 ms indirect line, with GI |

  These go to the phase review with the earlier overruns (ADR-0034 to
  ADR-0039). The resolve is the largest gap, and §8.6.5's per-material
  tile classification is the planned lever. Per CLAUDE.md §10.4 nothing
  is lowered to close the gap.
- The GPU runner does not run the reference scenes yet. It would need the
  packs cooked by the cooker under test, a 23-minute cook of Bistro
  whenever the cooker's output changes. Decide how with the phase review.
- The landscape and the interior follow the same scheme. The Poly Haven
  plants are alpha-tested, which §8.6's content budgets exclude
  ("modeled geometry instead"). That scene needs a decision: model the
  foliage, or scope an exception.

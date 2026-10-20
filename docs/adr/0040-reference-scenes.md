# 0040. Reference scenes: third-party content outside the repository, pinned by manifest

- **Date:** 2026-10-20
- **Status:** Accepted (Bistro measures 7.86 ms against the 4.17 ms frame:
  see Consequences; the review's decisions of 2026-10-20 at the end)

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
   - whether its sun casts shadows (`sun_shadows`): glTF has no such
     flag, and a game's sun casts them (the sun's virtual shadow map);
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
- **Bistro does not meet the target.** With its sun casting shadows, on
  the RTX 3060 Ti (Profile, 240 frames), it takes 7.86 ms mean and 8.70 ms
  at the 99th percentile, against 4.17 ms (docs/benchmarks.md):

  | Pass | Bistro | §10.4 line |
  |---|---|---|
  | Resolve | 3.87 ms | 1.20 ms |
  | Raster, 1.42 M triangles in 22,708 clusters | 1.14 ms | 0.65 ms |
  | Hi-Z, two phases (the first also gathers the sun's page requests) | 0.45 ms | inside the 0.25 ms culling line |
  | TAA | 0.85 ms | 0.40 ms, with post |
  | Ambient occlusion | 0.44 ms | 0.60 ms indirect line, with GI |

  The resolve, measured by taking one cost away at a time:

  | Cost | Share |
  |---|---|
  | The street lamps. 69 of the 97 lights are in view, and the grid hands each pixel 11.8 of them on average, of which 5.0 reach it | 1.25 ms |
  | Material textures (the normal map alone: 0.43 ms) | 0.84 ms |
  | Rebuilding the surface from the visibility buffer: records, vertices, barycentrics, attributes, the radiance-cache lookup | 0.97 ms |
  | The sun's virtual shadow map | 0.34 ms |

  These go to the phase review with the earlier overruns (ADR-0034 to
  ADR-0039). The lamps are the largest lever: 57% of the lights the loop
  evaluates are out of range. Per CLAUDE.md §10.4 nothing is lowered to
  close the gap.
- The GPU runner does not run the reference scenes yet. It would need the
  packs cooked by the cooker under test, a 23-minute cook of Bistro
  whenever the cooker's output changes. Decide how with the phase review.
- The landscape and the interior follow the same scheme. The Poly Haven
  plants are alpha-tested, which §8.6's content budgets exclude
  ("modeled geometry instead"). That scene needs a decision: model the
  foliage, or scope an exception.
- **The interior's candles cannot be lit faithfully by the grid.** Intel's
  pack is one 1 cd point light with no range (unlimited, by glTF's rule),
  placed by 10,000 nodes within 26 × 10 × 11 m, and one mesh of 19.5 M
  triangles. Giving the candles a range trades light for work. Summing the
  inverse-square light at points near the candles (no occlusion), the
  share beyond a range r, and the candles within r of a point, are:

  | r | Light beyond r (median) | Candles within r |
  |---|---|---|
  | 1 m | 79% | 33 |
  | 2 m | 61% | 184 |
  | 4 m | 37% | 929 |
  | 8 m | 13% | 4,225 |

  The far field is most of the light, and no range keeps it at a cost the
  light loop can pay. Rendering this scene faithfully needs stochastic
  many-light sampling (ReSTIR, §8.6 stage 10, which the roadmap puts in
  Phase 14), or a different interior. The renderer also holds at most
  4,096 lights. A decision for the phase review.

## Decisions at the review (2026-10-20)

The user decided the questions the consequences above left open:

- **The interior moves to Phase 14.** Sponza and its candles become the
  reference scene of ReSTIR's many-light sampling, which it needs.
- **§8.6's "no alpha-tested foliage" governs our own content.**
  Third-party reference scenes are measured as their authors made them.
- **The landscape moves to Phase 13.** Poly Haven's foliage is hero assets
  without levels of detail: its pine tree is 17.4 M triangles (a 950 MB
  glTF), its fir sapling 433 K, a grass clump 1.6 M. Any forest of them
  puts hundreds of millions of triangles in view, which only virtualized
  geometry's levels of detail (Phase 13) bound by pixels. Each reference
  scene now joins with the feature it stresses, so Phase 10's is the
  street (CLAUDE.md §10.7 and §12 say so now).
- **The §10.4 table stands.** The work goes on against it: no line is
  raised to meet a measurement, and no quality is given up to meet a line.
- **The GPU runner measures the reference scenes nightly**, cooking them
  there (incrementally, so Bistro's 23 minutes recur only when the
  cooker's output changes), not on every pull request.

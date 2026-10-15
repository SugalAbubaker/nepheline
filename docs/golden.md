# Golden-image tests (`tests/golden`, `tools/golden`)

CLAUDE.md §9: "render test scenes headlessly (lavapipe in CI, real GPUs
locally) and compare with NVIDIA FLIP (Andersson et al., HPG 2020) against
approved baselines". These tests are Phase 7's exit criterion ("golden-image
tests pass for the PBR test scenes"). Decisions are recorded in
[ADR-0019](adr/0019-golden-image-tests.md).

## What runs

`golden_tests` (CTest label `golden`) renders eight scenes with the
reference renderer (docs/render.md). Each is 256 × 256, on a headless
device, into an `R8G8B8A8_SRGB` target. It reads each image back, writes
it to `build/<preset>/golden/<Config>/<name>.png`, and compares it with
`tests/golden/baselines/<name>.png`:

| Scene | What it guards |
|---|---|
| `material_grid` | GGX across metallic × roughness, image-based lighting, a directional light, ACES |
| `punctual_lights` | point and spot lights in colour: inverse-square falloff, range windows, cone edges |
| `tonemap_ramp_aces` | the ACES fit over 2⁻⁸…2⁷, in white and three saturated hues |
| `tonemap_ramp_agx` | the same through AgX |
| `equirect_environment` | IBL from an equirectangular texture: prefiltering, SH irradiance, the background |
| `debug_draw` | every debug-draw shape, depth-tested and overlay |
| `cooked_scene` | `pbr_viewer`'s scene through the cooker, loader, and bridge: every material feature, AgX |
| `cooked_scene_normals` | the same scene's shading normals: normal maps, mirroring, two-channel maps |

A test passes when **mean FLIP ≤ 0.03** and **99th-percentile FLIP ≤
0.15** (see "Calibration"). The test always writes the render. When a baseline exists, it also
writes the FLIP error map (`<name>.flip.png`, magma colour map: black is
identical, yellow is maximally different) and prints the statistics. With
no baseline, the test fails and says how to approve one.

The fixture keeps the GPU tests' gate: zero validation and
synchronisation-validation errors, and nothing leaked.

## FLIP

`tools/golden/flip.c` is our implementation of the paper's LDR pipeline at
67 pixels per degree (a 0.7 m, 3840-pixel-wide display seen from 0.7 m):

- **colour:** YyCxCz; per-channel contrast-sensitivity filters (sums of
  Gaussians, applied separably); gamut clamp; Hunt-adjusted L\*a\*b\*;
  HyAB distance raised to 0.7, then the two-segment remap through
  (0.4 · cmax, 0.95);
- **features:** edges and points of the achromatic channel, from first and
  second derivatives of a Gaussian of the human edge-detection width
  (0.082°);
- **combination:** colour^(1 − features).

It was checked against NVIDIA's own implementation (`flip-evaluator` 1.7,
BSD-3-Clause), installed in a throwaway virtual environment; it is not a
dependency. On two 1280 × 720 renders of the `pbr_viewer` scene (ACES
against AgX), every pixel agreed within 4 × 10⁻⁵, and the means within
3 × 10⁻⁶. `tools/golden/tests/test_flip.c` locks in the authors' values
on six synthetic pairs (from `make_flip_expectations.py`).

`nph_flip REF.png TEST.png [--map=OUT.png]` compares any two PNGs.

## PNG

The baselines are PNG, so reviewers see them in any viewer and on GitHub.
The engine has no image codec, and stb_image is reserved for the cooker
(CLAUDE.md §3), so `tools/golden/png.c` is ours:

- **reading:** 8-bit grey, grey + alpha, RGB, and RGBA; all filter types;
  CRCs checked; our own inflate (stored, fixed, and dynamic Huffman
  blocks), tested against vectors made by Python's zlib;
- **writing:** stored deflate blocks.

The reader is fuzzed (`tests/fuzz/fuzz_png.c`), including an
encode/decode round trip.

## Baselines and approval

Baselines are rendered by **lavapipe**, the CI's Mesa software rasteriser.
It is deterministic, available to every contributor through CI, and
independent of anyone's GPU. They live in `tests/golden/baselines/` as Git
LFS objects (`.gitattributes`). CI's Linux jobs fetch them (the LFS store
is cached by object list) and upload every render and FLIP map as the
artifact `golden-<job>-<config>`.

To add or change a scene:

1. Change the scene and push. The golden test fails (or passes, if the
   change is invisible).
2. Download the Linux Development job's artifact:
   `gh run download <run> -n golden-linux-gcc-development -D /tmp/golden`.
3. Look at every new or changed image, and at its FLIP map against the old
   baseline. Approving is a review: the renderer is the reference the
   production path is measured against.
4. `python scripts/golden_approve.py --from /tmp/golden/Development NAME...`
   (or `--all`) copies them into `tests/golden/baselines/` and reports
   which are new, changed, or unchanged. Commit them with the change that
   caused them.

Locally, the same tests run on your GPU against the same baselines; see
"Calibration" for how far a real GPU may sit from lavapipe.

## Calibration

The thresholds must pass a correct renderer on any conforming GPU and
fail a real regression. Both sides were measured (2026-10-15).

**Determinism.** Rendering the same scene twice on one device gives
identical bytes. On lavapipe, the GCC and clang builds give identical
bytes in Debug and Development. Shipping differs by a mean FLIP of at most
0.003 (CPU-side float rounding under LTO).

**A real GPU against the lavapipe baselines** (RTX 3060 Ti, driver
610.74, Development):

| Scene | Mean | p99 | Max |
|---|---|---|---|
| material_grid | 0.0081 | 0.032 | 0.045 |
| punctual_lights | 0.0037 | 0.016 | 0.027 |
| tonemap_ramp_aces | 0.0010 | 0.033 | 0.060 |
| tonemap_ramp_agx | 0.0014 | 0.035 | 0.056 |
| equirect_environment | 0.0090 | 0.026 | 0.047 |
| debug_draw | 0.0052 | 0.041 | 0.333 |
| cooked_scene | 0.0106 | 0.039 | 0.208 |
| cooked_scene_normals | 0.0210 | 0.051 | 0.310 |

The differences are last-bit rounding: in `cooked_scene_normals`, 70 % of
channel values are identical, 30 % differ by one 8-bit step, and 0.5 %
by more (at edges and in the grate's holes). FLIP scores a one-step
difference of the normal view's light colours at about 0.02.

**Regressions at golden size.** The same scenes rendered 3.5 % brighter
(EV100 −0.05) score a mean of 0.036 to 0.062 in every lit scene. The
normal view is rightly unaffected.

The thresholds sit between the two: **mean ≤ 0.03** passes every GPU
measured and fails a brightness change of about 2 % or more; **p99 ≤
0.15** is three times the cross-device p99. It fails a local break the
mean dilutes: a moved object, or a lost normal map or light on part of
the image (a 0.23° camera move scores a p99 of 0.20). Exact radiance is
checked more tightly elsewhere: `render_tests` compare individual pixels
with the BRDF reference within two 8-bit steps.

Sensitivity of FLIP itself, measured on the RTX 3060 Ti with
`pbr_viewer`'s scene at 1280 × 720 (mean FLIP against an unchanged
render):

| Change | Mean | p99 |
|---|---|---|
| the same render again | 0 | 0 |
| 3.5 % brighter (EV100 −0.05) | 0.060 | 0.075 |
| 11 % brighter (EV100 −0.15) | 0.130 | 0.150 |
| 41 % brighter (EV100 −0.5) | 0.298 | 0.352 |
| AgX instead of ACES | 0.297 | 0.438 |
| the camera 0.23° further round its orbit | 0.026 | 0.202 |

## Cost

One comparison (FLIP of a 256 × 256 pair) takes 49 ms on the reference
CPU; the whole suite renders and compares in about 2 s on the RTX 3060 Ti
and 2 s on lavapipe. `render_bench` tracks FLIP, the PNG codec, and the
primitives (benchmarks/budgets.txt).

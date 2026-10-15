# 0019. Golden-image tests: our own LDR-FLIP and PNG codec, lavapipe baselines, calibrated thresholds

- **Date:** 2026-10-15
- **Status:** Accepted

## Context

CLAUDE.md §9 asks for "GPU golden-image tests: render test scenes headlessly
(lavapipe in CI, real GPUs locally) and compare with NVIDIA FLIP (Andersson
et al., HPG 2020) against approved baselines". Phase 7 ends when "golden-image
tests pass for the PBR test scenes" (§12). §3 limits third-party code to
a fixed list (stb_image only in the cooker, and no image library or FLIP
binary), and §1.2 requires everything to be tested. Baselines must be
reviewable by a person, survive repository history, and work on any
contributor's GPU as well as in CI, where the only device is Mesa's
software rasteriser.

## Decision

1. **FLIP is ours.** `tools/golden/flip.c` implements the paper's LDR
   pipeline at 67 ppd, with separable filtering and replicated borders.
   It is verified against NVIDIA's own `flip-evaluator` 1.7: to within
   4 × 10⁻⁵ per pixel on 1280 × 720 renders, and to within 10⁻⁴ on the
   six synthetic pairs whose expected values the unit tests hold.
   `flip-evaluator` was a throwaway verification tool
   (`make_flip_expectations.py`), never a dependency.
2. **PNG is ours.** Baselines must be viewable anywhere, and stb_image
   belongs to the cooker alone, so `tools/golden/png.c` reads 8-bit
   PNGs with our own inflate and writes stored-block PNGs. The reader is
   fuzzed. The engine still has no image codec; this lives in `tools/`.
3. **Baselines are rendered by lavapipe**, the device every contributor
   reaches through CI. Lavapipe is deterministic: GCC and clang builds
   produce identical bytes. Baselines are approved from a CI artifact
   with `scripts/golden_approve.py` after a person has looked at the
   images and their FLIP maps. They are 256 × 256 RGB PNGs in
   `tests/golden/baselines/`, stored with Git LFS, which CI fetches with a
   cache keyed on the object list.
4. **One pair of thresholds for every device: mean FLIP ≤ 0.03 and p99 ≤
   0.15.** They are calibrated, not guessed (docs/golden.md
   "Calibration"). The RTX 3060 Ti against lavapipe reaches at most a
   mean of 0.021 and a p99 of 0.052 (last-bit rounding). A 3.5 %
   brightness regression scores a mean of 0.036–0.062 in every lit scene,
   and a local break scores a p99 near 0.2.
5. **Eight scenes**, chosen so that each renderer feature has an image
   whose failure points at it: the material grid, punctual lights, the
   two tonemap ramps, equirectangular IBL, debug draw, and the cooked PBR
   scene lit and as normals. That cooked scene also carries the cooker,
   the loader, and the asset-to-renderer bridge.
6. **Golden tests are a separate CTest entry** (label `golden`), so the
   coverage job's unit-test run is unaffected. A missing baseline fails
   the test with instructions, rather than skipping.

## Alternatives considered

- **Per-pixel tolerance (PSNR, maximum absolute difference).** Simple, but
  blind to what a viewer notices. A one-pixel shift of an edge fails it,
  while a global hue shift may pass. FLIP is what §9 names, and it
  weights errors perceptually.
- **Wrapping NVIDIA's C++ FLIP.** It would add a dependency outside §3's
  list for about 400 lines of code, and the paper fully specifies the
  algorithm.
- **Baselines per device class** (lavapipe, NVIDIA, AMD, ...) with tight
  thresholds each. These are more sensitive, but multiply the
  approvals by the device count and require hardware no CI has. The
  measured cross-device gap is small enough for one set.
- **RTX 3060 Ti baselines.** The reference hardware renders the "truest"
  images, but CI cannot reproduce them while the self-hosted runner is
  down. Lavapipe images are reproducible by anyone.
- **Uncompressed or compressed formats other than PNG** (raw RGBA, QOI).
  Not viewable on GitHub or in common tools, which makes review harder.

## Consequences

- A rendering change that alters images must re-approve baselines in the
  same PR. Reviewers see the new PNGs in the diff (GitHub renders LFS
  images) and the FLIP maps in the CI artifact.
- A Mesa update on the CI image that changes rounding stays under the
  thresholds (it behaves like another GPU). A larger Mesa change would
  fail and call for re-approval, which is the right outcome.
- The thresholds miss a global change below about 2 %. Pixel-exact BRDF
  checks in `render_tests` cover that range.
- Baselines are 196 KiB each (stored deflate). LFS stores them, and the
  CI cache keeps bandwidth to new baselines only.
- The same harness serves the Phase 10 renderer: its images will be
  compared against this reference renderer's images, not only against
  baselines.

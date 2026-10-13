# 0015. GPU budget measurements on the RTX 3060 Ti and the §10.4 table

- **Date:** 2026-10-13
- **Status:** Accepted

## Context

CLAUDE.md §10.4 states the native-4K frame budget per pass in milliseconds
and DRAM bytes as design estimates, and §12 Phase 5 requires
microbenchmarks that measure achievable bandwidth and throughput on the
reference GPU so the table can be updated by ADR and enforced by CI
(§10.7). §10.2 derives the per-pixel figures from the board's peak numbers
(448 GB/s, 16.2 TFLOPS, 4 MB L2) with a 60 % margin on bandwidth.

`benchmarks/gpu` (docs/benchmarks.md) measures the engine's own primitives
through the gpu module with GPU timestamps: streaming copies, reads and
writes, dependent FMA chains, random 16-byte reads over working sets from
1 to 64 MiB, a 4K clear, full-screen raster fills into the formats the
renderer will use, the 8-byte-per-pixel visibility-buffer write, and one
trilinear texture sample per pixel at 4K.

## Measurements

RTX 3060 Ti (8 GB, driver 610.74, Vulkan 1.4.341), Profile build, median
of 20 runs after the clock warm-up described under Decision 4, from
`benchmarks/results/gpu-2026-10-13-win-msvc-profile-rtx3060ti.json`. Two
runs taken minutes apart agree within 0.5 % on every entry.

| Benchmark | GPU time | Metric |
|---|---|---|
| copy_256mib (`vkCmdCopyBuffer`, read + write counted) | 1.335 ms | 402 GB/s |
| write_256mib (compute, 16 B per thread) | 0.635 ms | 423 GB/s |
| read_256mib (compute, coalesced) | 0.621 ms | 433 GB/s |
| fma_fp32 (2 dependent vec4 chains per thread) | 1.297 ms | 13.2 TFLOPS |
| random_read_1mib / 2 / 4 / 8 MiB working set | 1.23 / 1.49 / 1.62 / 1.96 ms | 876 / 720 / 664 / 547 GB/s |
| random_read_16mib / 64 MiB | 4.93 / 8.37 ms | 218 / 128 GB/s |
| clear_4k_rgba8 | 0.067 ms | 499 GB/s |
| raster_fill_4k_rgba8_x8 (8× overdraw) | 0.480 ms | 138 Gpix/s |
| raster_fill_4k_r11g11b10_x8 | 0.480 ms | 138 Gpix/s |
| raster_fill_4k_rgba16f_x8 | 0.926 ms | 71.7 Gpix/s |
| visbuffer_write_4k_8bpp (R32G32_UINT, one layer) | 0.118 ms | 70.4 Gpix/s |
| texture_sample_4k_rgba8 (trilinear, 1 texel per pixel) | 0.280 ms | 29.7 Gpix/s |
| timestamp_resolution | 0.000 ms | 1 ns period |

## Decision

1. **The §10.4 pass budgets stand.** Sequential DRAM traffic reaches 402
   to 433 GB/s (90 to 97 % of the 448 GB/s peak), so a 4.17 ms frame can
   move about 1.7 to 1.8 GB; the table's ≤ 900 MB is about 50 % of what
   the memory system delivers, a larger margin than the 60 % of §10.2
   assumed, and every per-pass byte budget fits its time budget at the
   measured rate (the visibility-buffer pass: 200 MB in 0.65 ms needs
   308 GB/s; the shading pass: 250 MB in 1.20 ms needs 208 GB/s). Nothing
   is physically unreachable; the table is not lowered.
2. **Measured ceilings are recorded next to the estimates** and the
   design rules that follow from them:
   - *Random access costs a third of streaming.* 16-byte dependent random
     reads over a 16 MiB or larger working set run at 128 to 218 GB/s.
     Scattered material and geometry fetches in the visibility-buffer
     resolve must be counted at that rate, which is why the resolve is
     tile-binned by material (§8.6.5) rather than per pixel.
   - *The cache cliff lies between 8 and 16 MiB* on this GPU (4 MB L2 plus
     reuse): working sets up to 8 MiB keep 547 to 876 GB/s. The §10.2 rule
     of ≤ 2 MB per tile working set remains, with headroom.
   - *Sustained FP32 is 13.2 TFLOPS* (81 % of peak, dependent chains):
     55 GFLOP per frame, about 6,600 FLOP per 4K pixel. §10.2's 7,000 is
     the peak-derived number; budgets are written against 6,600.
   - *Raster output floors:* one full-screen 4K write of a 32-bit format
     costs 0.060 ms and of the 8-byte visibility buffer 0.118 ms; the
     0.65 ms visibility-buffer budget therefore allows about 5.5
     full-screen-equivalents of hardware output (overdraw plus the
     software rasteriser's share). RGBA16F fills at half the pixel rate
     of R11G11B10 and RGBA8, which is the §10.3.5 format rule in numbers.
   - *Texturing:* one trilinear RGBA8 sample per pixel at 4K costs
     0.28 ms, so the 1.20 ms fused shading budget holds roughly four
     uncompressed samples per pixel; BC-compressed virtual textures
     (4 to 8 bits per texel) are what make the material count viable.
   - *Clear and present:* a 4K RGBA8 clear is 0.067 ms, consistent with
     the 0.10 ms UI + present line.
3. **CI enforcement starts with the microbenchmarks.**
   `benchmarks/gpu_budgets.txt` holds each benchmark's reference time plus
   the 5 % allowance of §10.7 (and a 50 µs floor for the sub-0.2 ms
   entries); `scripts/gpu_budgets.py` generates it from the committed
   JSON, so a re-baseline is a new reference run, a new JSON under
   `benchmarks/results/`, a run of the script, and an ADR recording the
   change. `.github/workflows/gpu-perf.yml` runs the check on the
   self-hosted runner nightly, on pushes to `main`, and on pull requests
   that touch the gpu or renderer code, and fails on any exceedance. The
   per-pass table becomes CI-enforced in Phase 10, when the passes and the
   reference scenes exist (§10.7); until then this ADR is the record.
4. **The measurement method is the engine's own path**: timestamps
   written with `nph_gpu_cmd_write_timestamp`, read through the frame
   mechanism two frames later, each benchmark as one command buffer per
   frame, median over 20 runs with the first iteration discarded.
   Validation is off in the benchmark build. The GPU is driven to its
   sustained clock state before anything is measured: one second of FMA
   load before the suite, then at least 150 ms of each benchmark's own
   workload before its measured frames. The whole suite is only a few
   hundred milliseconds of GPU time, and without the warm-up the clock
   governor held the RTX 3060 Ti at 1665 MHz against 1920 MHz sustained
   (`nvidia-smi`), which made the raster fills 15 % and the random reads
   13 % slower than the numbers above and moved them between otherwise
   identical runs.

## Alternatives considered

- **Lower the §10.4 budgets to the measured 60 %.** The measured margin is
  wider than planned; lowering budgets would discard headroom the design
  may need on less capable GPUs.
- **Count random access at streaming rate.** The measurements show a 3×
  gap; budgets for scattered reads would be fiction.
- **Peak-derived FLOP figures.** 16.2 TFLOPS needs independent FMAs in
  every lane; real shading code has dependent chains. The sustained
  number is the honest one.
- **Measure without the clock warm-up.** The first runs were taken that
  way and the numbers moved by up to 15 % with the clock governor. Budgets
  derived from them would have carried that jitter in both directions:
  failing good changes on a cold run, and passing regressions hidden by a
  warm one.
- **A vendor profiler (Nsight) instead of our own timestamps.** Used for
  investigation, not for CI: the gate must run unattended on the runner
  through the engine's own API.

## Consequences

- The gpu-perf workflow gains push, pull-request, and nightly triggers
  and the budget check; it runs only on the repository's own branches.
- Renderer PRs (Phase 7 onwards) state their cost in milliseconds and
  bytes against the ceilings above (§10.7).
- The benchmark set grows with the renderer: every §10.4 pass gets a
  microbenchmark of its own primitive before the pass is written.

# Benchmarks

`benchmarks/` holds the CPU micro-benchmarks and `nbench`, the harness they
run on (CLAUDE.md §1.5 "measure before optimizing, and measure after", §9
"performance regression"), and the GPU microbenchmarks, which have a
harness of their own on the same JSON schema ([GPU benchmarks](#gpu-benchmarks)).

## Writing a benchmark

```c
#include <nbench/nbench.h>

NBENCH(alloc_free_64B)
{
    nbench_set_label(state, "tlsf");              // optional, stored in the JSON
    nbench_set_items_per_iteration(state, 1);     // ops per loop body (default 1)
    while (nbench_keep_running(state)) {
        void *p = nph_tlsf_alloc(&heap, 64, 16);
        nbench_do_not_optimize(p);                // keep the optimiser honest
        nph_tlsf_free(&heap, p);
    }
}
```

- `nbench_keep_running` starts the timer on the first call and stops it when
  the iteration budget is spent; everything inside the loop is measured.
  `nbench_pause_timing`/`nbench_resume_timing` exclude per-iteration setup.
- `nbench_set_items_per_iteration(n)` makes one loop body count as `n`
  operations, for workloads that batch (allocate 4096 blocks, free them all).
- `nbench_do_not_optimize(p)` / `_u64(v)` and `nbench_clobber_memory()` are
  the usual escape hatches so the compiler cannot delete the work.
- Register the executable with `nph_add_benchmark(<name> SOURCES … DEPS …)`,
  which builds `<name>_bench` and adds a 5 ms-per-benchmark smoke run to
  CTest under the `benchmark` label so benchmarks cannot rot.

## Methodology

For each benchmark the harness first calibrates the iteration count until a
single run lasts at least `--min-time` seconds (default 0.2 s), then takes
`--repeats` measured runs (default 5). The **median** ns/op over repeats is
the headline number; the minimum and maximum are reported alongside. Medians
resist the occasional scheduler interruption; the min is the best estimate
of the code's own cost.

```
core_alloc_bench [--filter=GLOB] [--min-time=SEC] [--repeats=N] [--json=FILE] [--list]
```

The JSON (`schema` 1) records the configuration, host OS/arch/compiler,
and per benchmark `name`, `label`, `iterations`, `items_per_iteration`,
`repeats`, `ns_per_op`, `min_ns_per_op`, `max_ns_per_op`.

Benchmarks are run in the **Profile** configuration: optimised, no asserts,
no sanitizers, profiler on, which is the configuration whose numbers the
§10 budgets are stated in. Development numbers include assert overhead and
are only useful relative to each other.

## Where results live

- `benchmarks/results/` — committed JSON snapshots from the reference
  machine, named `<executable>-<date>-<preset>-<config>-<cpu>.json`. Each
  module's design doc quotes the numbers that justify its decisions
  (`docs/core.md` for the allocators).
- The nightly **Benchmarks** workflow (`.github/workflows/benchmarks.yml`)
  runs every nbench executable on the hosted Windows and Linux runners and
  uploads the JSON as artifacts; `gpu_bench` is left to the self-hosted
  runner (below). Hosted runners are shared hardware; those numbers show
  trends, not absolutes.

## Budgets

`benchmarks/budgets.txt` lists a maximum median ns/op per benchmark, one
`name max_ns` pair per line (`#` comments). `--budgets=FILE` compares every
benchmark that ran against it after the run and exits with status 4 when
any exceeds its budget, which fails the nightly workflow (§9). The file is
plain text rather than JSON so that nbench stays free of engine
dependencies, like ntest.

Budgets are set at roughly three times the recorded reference-machine
number: hosted runners are slower and noisier, and the budget exists to
catch regressions of integer factors, not percentage drift. A benchmark
that blows through its budget on one hosted CPU model only usually has a
microarchitectural cause worth fixing rather than a budget to raise:
`lz_decompress_zeros` ran 4.5x slower on some runners because each copy
step waited on store-to-load forwarding (docs/asset.md). The workflow
prints each runner's CPU model before the run for this reason. The GPU
budgets are the tight kind: measured on the reference machine and enforced
there, within 5 % (below).

## GPU benchmarks

`benchmarks/gpu/bench_gpu.c` builds `gpu_bench`, a separate harness: nbench
times CPU loops, whereas GPU work is timed between pairs of
`nph_gpu_cmd_write_timestamp` read back through the frame mechanism
(docs/gpu.md). Each benchmark records one command buffer per frame between
two timestamps and reports the median, minimum, and maximum GPU time over
`--repeats` measured frames (default 10), plus a
derived metric: GB/s from the bytes moved, Gpix/s from the pixels written,
TFLOPS from the FLOPs issued. Validation is off; the numbers are the
driver's and the hardware's. The GPU is driven to its sustained clock state
first: one second of FMA load before the suite, then at least 150 ms of each
benchmark's own workload before its measured frames. The suite is only a
few hundred milliseconds of GPU time, and short bursts leave the clock
governor at a low boost step (1665 MHz observed against ~1900 MHz
sustained), which moved the raster numbers by 15 % between runs before the
warm-up existed.

| Benchmark | What it measures |
|---|---|
| `copy_256mib` | `vkCmdCopyBuffer` of 256 MiB (read and write both counted) |
| `write_256mib`, `read_256mib` | compute streaming through buffer device addresses, 16 B per thread |
| `fma_fp32` | dependent FMA chains: the sustained FP32 rate |
| `random_read_{1,2,4,8,16,64}mib` | dependent random 16 B reads over a working set of that size: the cache hierarchy |
| `clear_4k_rgba8` | `vkCmdClearColorImage` on a 3840×2160 RGBA8 image |
| `raster_fill_4k_{rgba8,r11g11b10,rgba16f}_x8` | eight full-screen triangles at 4K into each format: raster output rate |
| `visbuffer_write_4k_8bpp` | one full-screen R32G32_UINT write: the visibility buffer's floor |
| `texture_sample_4k_rgba8` | one trilinear sample per pixel at 4K from a mipmapped 4096² texture |
| `tonemap_4k` | the tonemap pass alone: per pixel one fetch of a B10G11R11 image, the ACES fit and the sRGB curve, one RGBA8 write |
| `taa_fragment_4k`, `taa_fragment_quad_4k`, `taa_fragment_cross_4k` | the production path's temporal resolve fused with the tonemap (`shaders/render/taa_common.glsl`), over noise: the neighbourhoods of the HDR colour and of the visibility buffer's depth (nine fetches each; four, shared across each 2 × 2 quad by subgroup quad swaps; or the five of the cross), the history's five Catmull-Rom taps, and two outputs, the display colour and the new history in B10G11R11; `_rgba16f` with the history in RGBA16F |
| `taa_compute_4k` | the same resolve as a compute pass of 16 × 16 groups reading the neighbourhoods from shared memory, then the tonemap pass; `_rgba16f` likewise |
| `timestamp_resolution` | an empty command buffer: the measurement floor |

```
gpu_bench [--filter=GLOB] [--repeats=N] [--json=FILE] [--budgets=FILE] [--list]
```

Exit codes follow nbench (0, 2 usage, 3 nothing matched, 4 budget exceeded)
plus 77 when no Vulkan device exists, which the CTest smoke run `bench.gpu`
reports as skipped. The JSON is the nbench schema with `metric` and `unit`
per entry and the GPU, driver, and API version under `host`.

The reference results are
`benchmarks/results/gpu-2026-10-18-win-msvc-profile-rtx3060ti.json` (the
temporal benchmarks added, the rest within 1 % of the first reference run,
`gpu-2026-10-13`); what they say about the CLAUDE.md §10 budgets is
recorded in [ADR-0015](adr/0015-gpu-budget-measurements.md) and, for the
temporal resolve, [ADR-0034](adr/0034-temporal-anti-aliasing.md). `benchmarks/gpu_budgets.txt`
holds each benchmark's reference median plus the 5 % regression allowance
of §10.7 (with a 50 µs floor on the sub-0.2 ms entries); it is generated
from the reference JSON by `scripts/gpu_budgets.py`, so a re-baseline (a
new driver, a new reference run) is a run of `gpu_bench --repeats=20
--json=...` on the reference machine, the JSON committed under
`benchmarks/results/`, the script run, and an ADR recording the change.
`.github/workflows/gpu-perf.yml` enforces the budgets on the self-hosted
RTX 3060 Ti runner. The hosted runners never run `gpu_bench` for real:
they have no GPU.

The reference machine runs NVIDIA driver 610.74 with the NVIDIA App's
overlay off and the driver's power management mode at "Prefer maximum
performance" for `gpu_bench.exe` and `render_perf.exe` (NVIDIA Control
Panel, Manage 3D settings, Program Settings; the profile matches the
executable's name, so the runner's own builds get it too). With the
default mode the GPU idled through the light scenes at 780 to 1,140 MHz:
the 512 × 512 grids measured 0.60 and 0.72 ms against 0.33 and 0.39. On
driver 621.01 with the overlay on (2026-10-21), the renderer's resolve and
the occlusion's denoise slowed two- to fourfold for 20 frames some four
seconds after rendering began, then ran 0.3 to 0.7 ms slow for three
seconds, and again at random later; none of the runs since, on 610.74
without the overlay, has shown it. The two drivers also allocate the
resolve's registers differently, so shader changes measured on one need
measuring on the other.

## Renderer runs

`benchmarks/render/render_perf.c` builds `render_perf`, which drives the
renderer itself on a device, scene by scene, and reports what CLAUDE.md
§10.7 asks every renderer PR to state:

- `<scene>.cpu`: the median CPU time of `nph_render_frame` alone (the
  records, the draw list, the pass declarations; not the render graph's
  compile and submit). Phase 10's exit criterion holds it constant in the
  object count on the production path.
- `<scene>.gpu` and `<scene>.gpu.p99`: the frame's mean and 99th-percentile
  GPU time (§10.1 measures the 1 % low).
- `<scene>.gpu.<pass>`: each render-graph pass's mean GPU time, with its
  declared footprint as the metric (`MB declared`: the bytes of the
  resource ranges it reads and writes, render_graph.h). That is an upper
  bound on DRAM traffic, not a measurement: whole buffers count even when a
  GPU-driven pass touches a fraction of them (the production path's lists
  are sized for its capacities), and caches absorb re-reads.
- `<scene>.gpu.span`: the frame's span, from its first pass's beginning to
  its last one's end on any queue (the render graph's `begin_ns`): the sum
  of the passes while one queue runs them all, shorter where passes overlap
  on the async compute queue (docs/render.md "GPU profiler").
- `<scene>.gpu.<pass>.p99` and `<scene>.gpu.span.p99`: their 99th
  percentiles. The passes' percentiles do not add up to the frame's: a
  pass's slowest frames are not every pass's.

Each scene first renders its view held still until the GPU's clock has
settled: in windows of 64 frames, until three windows in a row each take
at least 99% of the one before (at most 10 s). A GPU idle through the
scene's load starts at an idle clock, and its boost back took from under
half a second to four seconds on the RTX 3060 Ti (driver 621.01,
2026-10-21); the 500 ms warm-up that was all there was then measured
Bistro's street on the way, 10.1 ms a frame against 7.1. The view is held
still so that a moving scene's measured path does not depend on how long
the clock took. Then come warm-up frames (at least 24 and 500 ms: instance
uploads, the environment's image-based-lighting passes, the scene's
motion) and `--frames` measured ones (default 120), timed by the
renderer's GPU profiler. Every scene draws the runtime's bloom (0.04,
docs/render.md "Bloom and exposure") with the scene's own exposure, and
the production path's scenes resolve temporally (the runtime's default,
"Temporal anti-aliasing"), take the runtime's global illumination ("Global
illumination") and, where the sky lights them, its ambient occlusion
(0.5 m, "Ambient occlusion"). The scenes are synthetic until Phase 10's
reference scenes arrive:

| Scene | What it is |
|---|---|
| `grid_{1k,10k,100k}_{reference,production}` | that many cubes on a flat grid under a top-down camera, every one on screen, at 512×512: the CPU cost against the object count, per pipeline |
| `field_4k_{reference,production}` | 16,384 spheres (960 triangles each) and pillars on a ground plane at 3840×2160 from head height: the nearer rows hide most of the field |
| `field_4k_no_taa` | `field_4k_production` without temporal anti-aliasing: the tonemap pass where the other has the temporal resolve |
| `field_4k_no_bloom` | `field_4k_production` without bloom: what the chain and its composite cost |
| `field_4k_no_ao` | `field_4k_production` without ambient occlusion: what the occlusion costs |
| `field_4k_no_gi` | `field_4k_production` without global illumination: what the radiance cache costs under the sky |
| `field_4k_auto_exposure` | `field_4k_production` exposed automatically: the histogram and the metering on top |
| `lights_1k_{reference,production}`, `lights_1k_every_light`, `lights_4k_production` | the field at night, lit by 1,024 (or 4,000) point and spot lamps of 3 to 8 m range among the nearer rows and a dim moon: the light grid against the light count; `every_light` is the production path lighting every pixel with every light |
| `shadows_4k_{reference,production}` | the field with the sun casting shadows (docs/render.md "Shadows"): the reference traces rays, the production path draws the sun's virtual shadow map |
| `shadows_4k_no_gi` | `shadows_4k_production` without global illumination: what the radiance cache costs with the sun's light to bounce |
| `shadows_rt_4k_production` | the production path with the sun's shadows ray traced instead (`NPH_RENDER_VIS_RT_SHADOWS`) |
| `shadows_moving_4k_production`, `shadows_walk_4k_production` | `shadows_4k_production` with one sphere moving every frame (the pages it touches are drawn again), or with the camera walking at 1.4 m/s at 240 frames a second (the pages it comes to need are drawn) |
| `lights_1k_shadows` | `lights_1k_production` with the moon and every lamp casting shadows |
| `motion_{turn,strafe,flick}_4k_production` | `field_4k_production` with motion blur (a 180-degree shutter at 240 frames a second) while the camera turns 60 degrees a second, strafes at 5 m/s (parallax), or turns 360 degrees a second, each reversing every quarter second (docs/render.md "Motion blur") |
| `dof_4k_production`, `dof_slight_4k_production`, `dof_large_4k_production` | `field_4k_production` through a full-frame lens (24.8 mm at the camera's field of view) at f/1.4 focused 5 m away (circles of confusion up to 4 pixels), 2.6 m (up to 7.7: the most the full-resolution gather takes), or 0.6 m (the 32-pixel limit, gathered at half resolution) (docs/render.md "Depth of field") |

`lights_1k_every_light` and `lights_1k_shadows` are comparisons, not
targets: like the reference renderer's scenes, they carry no budget.

```
render_perf [--filter=GLOB] [--frames=N] [--json=FILE] [--budgets=FILE] [--list] [--validation] [--sw-pixels=N]
            [--scenes=DIR] [--capture-dir=DIR] [--manifests=DIR] [--ref-size=WxH] [--debug-view=NAME] [--reference-only]
```

**Reference scenes** (CLAUDE.md §10.7: the targets are measured on them)
are third-party content, kept outside the repository: a manifest in
`benchmarks/scenes/` pins each one's source (a git repository and commit,
its document, its licence and attribution), names its cooked pack and
scene, its exposure (`ev100`), its sky (`sky_intensity`, scaling the
default daylight sky, which pairs with a 30,000 lux sun, to the scene's
own sun), whether its sun casts shadows (`sun_shadows`: glTF cannot say,
and a game's would), and its views (the scene's own camera, or an eye and
a target with a field of view; still, or moving: `walk` metres a second
along the view, level, and `turn` degrees a second about the world's up
axis, one step a frame as at 240 frames a second, the warm-up included so
the caches meet the motion in their steady state). `scripts/scenes.py fetch` checks the
source out at its commit under a directory of the machine's choosing and
`scripts/scenes.py cook` cooks it there (`nph_cook`, incremental); with
`--scenes=DIR` (or the `NPH_SCENES` environment variable) render_perf adds
a scene per view of every manifest whose pack DIR holds,
`<scene>_<view>_production`, after the synthetic ones: 3840 × 2160 through
the production path with the runtime's defaults, lit by the scene's own
lights (the synthetic scenes' sun is removed). `--capture-dir` writes
each one's last frame there as a BMP. `--debug-view` renders them as one
of the renderer's debug views (base colour, normals, roughness, ...) to
look at the content, under names no budget matches. Without the packs
those scenes are skipped with a message, so the budgets of a machine
without them are those of the synthetic scenes alone. The CTest smoke
run of the path (`bench.render_perf_reference`) cooks pbr_viewer's scene,
reads its manifest from `benchmarks/render/test_scenes` (`--manifests`),
and renders it at 320 × 180 (`--ref-size`) for four frames.

The RTX 3060 Ti runner measures them nightly, not on pull requests (the
user's decision, ADR-0040). `.github/workflows/gpu-perf.yml`, when
scheduled or dispatched, fetches and cooks every manifest's scene into a
directory that persists between its jobs. That directory is the
repository variable `NPH_SCENES_DIR`, or the runner's tool cache when the
variable is unset. The cook is incremental, so Bistro's 23-minute cook
recurs only when the cooker's output changes. The job then runs
`render_perf --reference-only` against `benchmarks/reference_budgets.txt`,
which `scripts/render_budgets.py --output` generates from the reference
results like the synthetic budgets.

| Scene | Source | What it is |
|---|---|---|
| `bistro` | Amazon Lumberyard Bistro, exterior (CC BY 4.0; NVIDIA's glTF conversion, MIT, as `zeux/niagara_bistro` edits it) | a street at dusk: 2,909 instances of 551 meshes, 254 materials (234 specular-glossiness) using 338 BC7 DDS textures, the sun at 6,830 lux and 96 street lamps |

Measured on the RTX 3060 Ti (Profile, 240 frames, 2026-10-22), against
the 4.17 ms frame of §10.1:

| Scene | GPU mean | p99 | Largest passes |
|---|---|---|---|
| `bistro_street_production` | 6.92 ms | 6.99 ms | resolve 2.81 ms (§10.4 line: 1.20 ms), AO 1.33 ms at full resolution (with the radiance cache, 0.60 ms), raster 0.74 ms + Hi-Z 0.30 ms (with culling, 0.89 ms), TAA 0.88 ms (with post, 0.40 ms) |
| `bistro_street_walk_production` | 6.97 ms | 7.07 ms | as the street: the shadow map's pages it comes to need cost 0.01 ms |
| `bistro_street_turn_production` | 7.15 ms | 7.71 ms | raster 1.12 ms: the turn sweeps over the foliage the next two views hold |
| `bistro_terrace_production` | 7.45 ms | 7.53 ms | raster 1.21 ms: alpha-tested cypresses beside the café |
| `bistro_hedge_production` | 7.07 ms | 7.14 ms | raster 1.38 ms: an alpha-tested hedge and cypresses across the view |

Bistro's manifest has five views: the scene's own camera still
(`street`), walking along the street at 1.4 m/s (`street_walk`), turning
90° a second from it (`street_turn`), and turned 50° and 90° (`terrace`,
`hedge`), still. The motion itself costs little: walking added 0.05 ms,
and stills along the turn's path cost what the turn does (their raster
1.75, 1.66, 2.15 and 1.11 ms at 50°, 70°, 90° and 110°). What comes into
view decides, and the two stills hold the foliage that costs most: the
masked fragments' alpha test, which the mesh stage now hands them
(docs/render.md "Masked clusters"), took their frames from 8.14 and 7.72
ms (p99 9.25 and 8.53) to 7.96 and 7.42. Then phase 1's large clusters
stopped asking for a whole second pyramid (docs/render.md "The depth
pyramid"), which the terrace had rebuilt on most frames and the street on
every fourth.

The p99 includes the machine as well as the renderer. Before the light
loop's terms were hoisted (docs/render.md "Lights"), every large pass of a
still view (the resolve, the raster, TAA, the occlusion, even the 0.05 ms
software raster) now and then ran about 0.5 ms longer than it otherwise
did, in bursts, with the NVIDIA overlay on or off; since then a still
view's p99 is within 0.1 to 0.2 ms of its mean. The turning view's wider
spread is its own: what comes into view changes.

The visible geometry is 1.42 M triangles in 22,708 clusters (2,687 of
them rasterised in software). 69 of the 97 lights are in view, and the
light grid hands each pixel 11.8 of them on average, of which 5.0 reach
it. The resolve carries most of the overrun. Taking one cost away at a
time (ADR-0040): the lamps 1.25 ms, material textures 0.84 ms, rebuilding
the surface from the visibility buffer 0.97 ms, the sun's virtual shadow
map 0.34 ms. Testing each light's range before reading the rest of it
(docs/render.md "Lights") then took 0.29 ms off the lamps' share, and the
hardware raster's early depth test (docs/render.md "Early depth") took
its pass from 1.13 to 0.74 ms. Testing the blended surfaces against the
visibility buffer's depth in their shaders then dropped the 0.19 ms pass
that copied it into a depth attachment for them, and building the depth
pyramid's mips six a dispatch took 0.03 ms off the second pyramid. The
frame varies by about 0.05 ms from run to run, and the nightly order,
which runs these after the synthetic scenes on a warm GPU, measures about
0.05 ms more: the rows are a run alone (`reference_scenes-2026-10-22e`).
The ambient occlusion at full resolution added 0.88 ms to the street's
frame against half resolution's (docs/render.md "Ambient occlusion"); the
light tiles' depth bounds, the resolve's push constants and the opaque
clusters' ROP raster then took 0.21 to 0.38 ms off each view, and the
masked skin (docs/render.md "Masked clusters") 0.33 to 0.54 ms off the
three foliage views, and the local lights' terms rearranged exactly
(docs/render.md "Lights") 0.04 to 0.15 ms off each view.
Settling the clock first and the reference machine's settings ("GPU
benchmarks" above) moved no view's mean by more than 0.04 ms.
Computing the light loop's light-independent terms once and reading the
resolve's records through references (docs/render.md "Lights") took 0.10
to 0.13 ms off each view, averaged over the runs of each, and the slow
frames with it (above).
Requesting a 2 × 2 block's shadow pages at once in the depth pyramid's
mip-0 pass (docs/render.md "The sun's virtual shadow map") took 0.07 to
0.09 ms off each view, the pass going from 0.38–0.39 to 0.30–0.31 ms.
Filling the resolve's surface in the order that ends its values' live
ranges, with the light loop's Fresnel and energy terms folded per pixel
(docs/render.md "Lights"), took 0.09 to 0.14 ms off each view.
Stepping the occlusion denoise's plane test along each row (docs/render.md
"Ambient occlusion") took 0.011 to 0.013 ms off the denoise, within the
frames' spread from run to run.

The device keeps its shader statistics, so the log (stderr) has each
production compute pass's registers and spills, and the temporal
resolve's, as the driver reports them.

Exit codes and the JSON follow `gpu_bench`. The CTest smoke run
(`bench.render_perf`) renders the 1k grids for four frames, checks the
JSON's shape (`benchmarks/render/check_results.py`: every GPU entry has
its 99th percentile beside it) and skips without a device. The reference
results are
`benchmarks/results/render_perf-2026-10-22e-win-msvc-profile-rtx3060ti.json`;
`benchmarks/render_budgets.txt`, generated from them by
`scripts/render_budgets.py`, budgets the production scenes only: GPU
entries at the reference mean plus 5 % (50 µs floor below 0.2 ms), CPU
entries at a flat 50 µs, which per-object work on the main thread would
break. `.github/workflows/gpu-perf.yml` enforces them on the RTX 3060 Ti
runner.

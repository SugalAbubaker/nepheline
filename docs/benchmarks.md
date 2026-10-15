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
catch regressions of integer factors, not percentage drift. The GPU
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
| `timestamp_resolution` | an empty command buffer: the measurement floor |

```
gpu_bench [--filter=GLOB] [--repeats=N] [--json=FILE] [--budgets=FILE] [--list]
```

Exit codes follow nbench (0, 2 usage, 3 nothing matched, 4 budget exceeded)
plus 77 when no Vulkan device exists, which the CTest smoke run `bench.gpu`
reports as skipped. The JSON is the nbench schema with `metric` and `unit`
per entry and the GPU, driver, and API version under `host`.

The reference results are
`benchmarks/results/gpu-2026-10-13-win-msvc-profile-rtx3060ti.json`; what
they say about the CLAUDE.md §10 budgets is recorded in
[ADR-0015](adr/0015-gpu-budget-measurements.md). `benchmarks/gpu_budgets.txt`
holds each benchmark's reference median plus the 5 % regression allowance
of §10.7 (with a 50 µs floor on the sub-0.2 ms entries); it is generated
from the reference JSON by `scripts/gpu_budgets.py`, so a re-baseline (a
new driver, a new reference run) is a run of `gpu_bench --repeats=20
--json=...` on the reference machine, the JSON committed under
`benchmarks/results/`, the script run, and an ADR recording the change.
`.github/workflows/gpu-perf.yml` enforces the budgets on the self-hosted
RTX 3060 Ti runner. The hosted runners never run `gpu_bench` for real:
they have no GPU.

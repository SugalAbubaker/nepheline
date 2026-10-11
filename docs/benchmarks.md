# Benchmarks

`benchmarks/` holds the CPU micro-benchmarks and `nbench`, the harness they
run on (CLAUDE.md §1.5 "measure before optimizing, and measure after", §9
"performance regression"). GPU benchmarks arrive in Phase 5 with their own
harness on the reference machine (ADR-0002).

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
  runs every benchmark on the hosted Windows and Linux runners and uploads
  the JSON as artifacts. Hosted runners are shared hardware; those numbers
  show trends, not absolutes.

## Budgets

`benchmarks/budgets.txt` lists a maximum median ns/op per benchmark, one
`name max_ns` pair per line (`#` comments). `--budgets=FILE` compares every
benchmark that ran against it after the run and exits with status 4 when
any exceeds its budget, which fails the nightly workflow (§9). The file is
plain text rather than JSON so that nbench stays free of engine
dependencies, like ntest.

Budgets are set at roughly three times the recorded reference-machine
number: hosted runners are slower and noisier, and the budget exists to
catch regressions of integer factors, not percentage drift. The
self-hosted machine (Phase 5) enforces the tight numbers.

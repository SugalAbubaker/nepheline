# 0081. Benchmark budgets in the machine's own unit

- **Date:** 2026-11-08
- **Status:** Accepted

## Context

The nightly Benchmarks workflow runs every nbench executable on GitHub's hosted Windows and Linux runners and fails when
a benchmark's median exceeds its line in `benchmarks/budgets.txt` (docs/benchmarks.md "Budgets"). A budget is about
three times the reference machine's number, which assumes that a hosted runner differs from the reference by less.

For the uncontended locks it does not. Four of the eight nightlies from 2026-11-01 to 11-08 went red on the platform
benchmark's locks, with nothing changed in `engine/platform`, and every red job ran on an Intel Xeon (median ns per
operation, bold over budget):

| Nightly | Runner | CPU | mutex | spin | semaphore post+wait | round trip |
|---|---|---|---|---|---|---|
| 10-28 | linux-gcc | Xeon Platinum 8370C | 7.43 | 7.78 | **18.84** | **316.4** |
| 11-02 | windows-msvc | Xeon Platinum 8573C | **18.59** | **9.64** | **17.04** | 95.2 |
| 11-04 | windows-msvc | Xeon Platinum 8573C | **18.89** | **10.39** | **22.81** | 96.7 |
| 11-04 | linux-gcc | Xeon Platinum 8370C | 7.36 | 7.79 | **18.77** | 218.7 |
| 11-08 | linux-clang | Xeon 6973P-C | 5.41 | 7.19 | **16.35** | **335.5** |
| | | (budgets) | 15 | 8 | 15 | 250 |
| 11-06 | windows-msvc | AMD EPYC 9V74 | 4.98 | 2.18 | 4.93 | 54.7 |
| 11-07 | windows-msvc | AMD EPYC 7763 | 6.20 | 2.79 | 5.90 | 83.9 |
| 11-07 | linux-gcc | AMD EPYC 9V45 | 4.50 | 5.60 | 10.74 | 60.5 |

On those Xeons a locked instruction costs three to five times what it does on the EPYCs most jobs get: the spin lock,
one exchange and a store, took 7.2 to 10.4 ns against 2.2 to 2.8. All seven repeats of a red job were over its budget,
the fastest included: the slowness is the machine's, so no statistic over the repeats removes it.

The locks pay the hardware's locked instructions and nothing else of ours: the spin lock is an exchange and a store,
the semaphore's post and wait a locked add and a compare-exchange, the mutex the operating system's (an SRW lock;
glibc's pthread mutex). docs/benchmarks.md had a rule for such a cost, made for the job system's `counter_signal`: its
budget follows the slowest runner. That rule holds the slow runners but loosens every other: at the slowest runner's
mutex, a mutex could take four times as long on an EPYC and pass.

## Decision

1. **Every nbench executable measures the machine's unit, `nbench.locked_add`:** one uncontended atomic add (a locked
   add on x86) to a cache line no other thread touches. It is the compiler's intrinsic, not the engine's wrapper, so a
   regression in the wrapper shows in the benchmarks the unit measures. It runs first and again after the executable's
   own benchmarks, and the faster counts: a neighbour busy on a hosted runner's shared core can only slow it, and a slow
   spell would loosen every budget stated in it (one job's `platform_bench` measured 11.83 ns where the job's other 14
   executables measured 6.05).
2. **A budget may be relative:** `name <k>x base` holds a benchmark to k times the median of benchmark `base` in the
   same run. A benchmark may have several budgets and must be within each. A base the executable does not have fails the
   check; a base filtered out of the run skips the budget, with a line saying so.
3. **The benchmarks bound by locked instructions are budgeted in the unit**, k about one and a half times the largest
   ratio measured over nine hosted jobs (three runs of the workflow on this change: four Intel Xeon jobs, five AMD EPYC)
   and the reference machine:

   | Benchmark | Ratio to the unit | Budget |
   |---|---|---|
   | `spin_lock_acquire_release` | 0.86 to 1.49 | 2.5x |
   | `semaphore_post_wait` | 1.78 to 3.16 | 5x |
   | `mutex_lock_unlock` | 0.91 to 4.32 | 7x |
   | `counter_signal` | 1.77 to 3.09 | 5x |

   The unit measured 1.5 ns on the reference machine (Ryzen 7 5800X), 1.9 to 2.4 on the EPYCs (7763, 9V74) and 6.1 to
   9.2 on the Xeons (Platinum 8370C and 8573C, 6973P-C). glibc's mutex is the widest: about 4 units on the EPYC 7763, 1
   on the Xeons.
4. **The unit's own budget follows the slowest runner** (15 ns): it is the hardware's cost.
5. **`semaphore_round_trip` keeps an absolute budget, the slowest runner's, 500 ns** (was 250). It hands a permit to
   another core and back, and waits on the interconnect between the two cores the OS picked, which no single-core unit
   measures: on one CPU model, the Xeon 6973P-C, it took 109 ns under Windows and 335 under Linux. The regression worth
   catching there, a hand-off that sleeps in the kernel, costs microseconds.

## Alternatives considered

- **The best of the repeats** (the minimum instead of the median): the minimum was over the budget too.
- **Budgets that follow the slowest runner** (`counter_signal`'s rule): they hold, but on the fast runners a lock could
  take four times as long and pass.
- **Budgets per CPU model:** a table to keep up with GitHub's fleet, failing on each new model until it is added.
- **Each benchmark against its own history on the same CPU model:** catches drift the most tightly, but the nightly
  would fetch past runs' artifacts and judge a new model by nothing; more machinery than a guard against integer
  factors needs.
- **The CPU benchmarks on the RTX runner**, a fixed machine: one machine to judge them on, but it is the user's PC, off
  on some nights, and its jobs are the GPU's.
- **A cross-core unit for the round trip** (a raw ping-pong between two threads): it waits on whichever two cores the OS
  picks, which a separate run does not share with the benchmark it would measure.

## Consequences

- The budgets of the atomic-bound benchmarks hold on every runner measured, and on the fast runners they catch smaller
  regressions than before: on an EPYC 9V74 the spin lock failed at 3.7 times its cost and now fails at 2.2, the
  semaphore's post and wait at 3.0 and now at 1.9.
- Each nbench executable spends a few seconds more on the unit, twice, and every results file records the machine's
  atomic cost beside its numbers.
- The ratios are those of the CPUs GitHub's runners had in November 2026: a CPU whose ratio is out of line fails the
  nightly, and that is a finding about the CPU.
- nbench has tests at last (`nbench.cli.*`): budget files against an executable whose two benchmarks do work in a known
  ratio, checked through ntest's `run_check.cmake`.
- Other benchmarks run close to their budgets on the Intel hosts without being bound by atomics: the physics worlds'
  `_mt` rows (84 to 100 %: four-vCPU runners and slower cores), `mix_ramp_scalar` (99 to 100 %, red once on 11-07) and
  `blue_noise_32x32` (93 %). They keep absolute budgets, under the rule that a budget blown on one CPU model has a cause
  to find first.

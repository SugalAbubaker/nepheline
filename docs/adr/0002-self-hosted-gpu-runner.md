# 0002. Self-hosted RTX 3060 Ti performance runner

- **Date:** 2026-10-11
- **Status:** Accepted

## Context

The performance target (CLAUDE.md §10) is stated for one reference GPU, the
NVIDIA RTX 3060 Ti, as 1%-low frame time and per-pass GPU timestamps plus
DRAM traffic. GitHub-hosted runners have no GPU; lavapipe runs the
correctness tests but cannot measure performance. §6 and §10.7 therefore call
for a self-hosted runner with the reference GPU that runs the reference
scenes nightly and on every renderer PR, failing CI on a >5 % regression.

The development machine already is the reference hardware: RTX 3060 Ti
(8 GB, driver 610.74), Ryzen 7 5800X, Windows 10. Registering a self-hosted
runner on a **public** repository carries a documented risk: workflows from
forks could execute arbitrary code on that machine.

## Decision

1. **Timing.** The runner is registered at the start of **Phase 5** (GPU/RHI),
   when the first GPU code and the bandwidth/throughput microbenchmarks exist.
   Nothing can be measured before then, and the machine is not exposed
   earlier than necessary. Phase 0 ships the workflow file and this ADR so
   the design is fixed now.
2. **Workflow.** `.github/workflows/gpu-perf.yml`, label set
   `[self-hosted, windows, x64, gpu, rtx3060ti]`. Until Phase 5 it is
   `workflow_dispatch` only. Phase 5 adds `push` to `main` and
   `pull_request` triggers and the §10.4 budget comparison.
3. **Fork isolation.** The job runs only when the PR head repository is this
   repository (`github.event.pull_request.head.repo.full_name ==
   github.repository`). In addition the repository setting *"Require approval
   for all outside collaborators"* stays on, and the runner is attached to the
   repository, never to the user account or an organisation.
4. **Runner hardening.** The GitHub Actions runner service runs under a
   dedicated standard (non-administrator) Windows user with access to its
   work folder only; no secrets are configured for it; it is started in
   ephemeral mode (`--ephemeral`, one job per registration, re-registered by a
   scheduled task) so a job cannot leave state for the next one.
5. **What it measures.** The Profile configuration of the reference scenes at
   native 3840×2160: GPU timestamps per render-graph pass and total frame
   time, compared with the §10.4 table (updated by ADR after Phase 5
   measurements). Results are stored as JSON artifacts and plotted over time.

## Alternatives considered

- **Cloud GPU runners** (AWS G4/G5, Azure NV, third-party CI GPU services) —
  expensive for nightly use, and none offers the exact reference GPU; a T4 or
  A10G would measure a different machine than the one the targets are
  written for.
- **Manual measurement only** — removes the exposure entirely but makes the
  §10 budgets advisory rather than enforced; regressions would be found late.
- **Register the runner now (Phase 0)** — no benefit before GPU code exists,
  and it extends the exposure window by several phases.
- **A separate dedicated machine** — the right long-term answer if the
  project gains contributors; today it would duplicate hardware we already own.

## Consequences

- From Phase 5, renderer PRs gain a required check that can only run when
  the development machine is on. The job has a 60-minute timeout and the
  check is scoped by path filter to renderer changes so unrelated PRs are not
  blocked.
- A single machine is a single point of failure and a single sample of
  silicon; variance is handled by taking 1%-low over multiple runs rather
  than a single frame time.
- Phase 5 must include the runner installation steps (service account,
  ephemeral re-registration task) in `docs/build.md`.

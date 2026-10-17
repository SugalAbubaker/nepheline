# 0028. The GPU runner as a standard runner service

- **Date:** 2026-10-17
- **Status:** Accepted. Replaces §4's ephemeral registration loop in
  [ADR-0002](0002-self-hosted-gpu-runner.md).

## Context

Phase 5 installed the RTX 3060 Ti runner as ADR-0002 §4 described. A
scheduled task running as SYSTEM looped forever. Each pass minted a
registration token with a stored fine-grained PAT (*Administration: read
and write*, DPAPI-protected under `C:\actions-runner\admin`) and registered
the runner ephemerally with `config.cmd --ephemeral`. It then started a
second task that ran `run.cmd` as the standard user `nph-runner` for one job.

On 2026-10-14 the machine restarted while the runner sat registered and
idle. The unused registration's `.runner` and `.credentials` files stayed in
the runner folder, and from then on every pass failed with "Cannot configure
the runner because it is already configured". GitHub listed no runners.
Every *GPU performance* run queued until it expired, and nothing flagged the
outage, because the check is not required. Any restart while idle would have
broken the loop again.

Investigating it showed that the isolation §4 promised did not hold:

- The SYSTEM loop ran `config.cmd` from `C:\actions-runner`. `nph-runner`
  can modify that folder by design, and Authenticated Users can too, through
  inheritance from `C:\`. A job could therefore replace `config.cmd` and get
  code executed as SYSTEM.
- Re-running the installer stamped an explicit `nph-runner` modify ACE onto
  the existing `admin` folder (`icacls /grant ... /T`), which
  `/inheritance:r` does not remove. The job's user could then rewrite the
  SYSTEM loop script and reach the PAT.

Ephemeral registration bought less than it appeared to. Every job runs as
the same persistent Windows user, in the same profile, on the same disk. A
fresh registration per job does not reset any of that state. Only a fresh
machine or VM per job would, and ADR-0002 already rejected that.

## Decision

1. **Standard runner service.** `scripts/runner/install.ps1` registers the
   runner once with `Runner.Listener configure --runasservice
   --windowslogonaccount <machine>\nph-runner`. The Windows service starts
   automatically at boot and keeps its registration across restarts and
   jobs, the way the runner is designed to be used. Its listener and every
   job run as `nph-runner`, a standard user with no administrator rights.
2. **No stored credential.** The installer uses one registration token
   (valid for an hour) and never writes it to disk. It gets the token from
   `-Token`, from `gh` when the administrator is logged in, or from a
   prompt. The PAT and the `admin` folder go away, and the old PAT is
   revoked.
3. **Nothing elevated executes job-writable files.** The loop and its
   scheduled tasks are gone. The installer deletes any earlier runner folder
   without running anything from it. It then creates `C:\actions-runner`
   with inheritance removed (Administrators and SYSTEM only), unpacks the
   release there after checking its SHA-256 against the hash GitHub
   publishes in the release notes, and runs the configuration from that
   untouched copy. Only after that does `nph-runner` get modify rights on
   the folder, for self-updates, diagnostics, and `_work`. From then on,
   the code in the folder runs only as `nph-runner`. Uninstalling stops
   and deletes the service with `sc.exe`, never `config.cmd remove`.
4. **Unchanged from ADR-0002:** the repository-scoped runner and its labels,
   the workflow's refusal of fork pull requests, *Require approval for all
   outside collaborators*, and no secrets available to the job.

## Alternatives considered

- **Keep the ephemeral loop and repair it.** That means deleting a stale
  registration at startup, fixing the ACLs, and running `config.cmd` as
  `nph-runner` with the SYSTEM loop only handing over tokens. It keeps a
  PAT that can administer the repository stored on the machine, plus a
  SYSTEM process that talks to a folder the job controls. That is more
  moving parts for the same isolation, since jobs share the user and the
  disk either way.
- **Ephemeral per-job VMs** (Hyper-V with GPU partitioning). This is the
  only design that really isolates jobs from each other. But GPU
  partitioning on a Windows 10 client is unofficial, and it would measure a
  virtualised GPU rather than the reference one.
- **A GitHub App instead of the PAT.** This narrows the credential but
  still stores one on the machine, and it does nothing about the staleness.

## Consequences

- A restart no longer takes the runner offline. The service reconnects
  with its existing registration.
- A job can read the runner's own registration credential (`.credentials`
  under the runner folder). With it, a job could impersonate this runner
  and pick up later jobs for this repository. Those jobs carry no secrets,
  and only this repository's own workflows reach the machine, so the
  exposure is no larger than "a job can run code as `nph-runner`", which
  every design accepts.
- State can persist between jobs in `_work` and in `nph-runner`'s profile.
  `actions/checkout` cleans the workspace, and the build directory is
  reconfigured every run.
- Re-running the installer is a clean reinstall: new password, new
  registration (`--replace` keeps the name), and a freshly downloaded
  runner.
- The administrator must revoke the old fine-grained PAT on GitHub. The
  installer can no longer reach it, and nothing uses it.

# Architecture Decision Records

Significant decisions are recorded here as numbered Markdown files
(CLAUDE.md §1.7). An accepted ADR is not edited afterwards; a later ADR
supersedes it and both link to each other.

| ADR | Title | Status |
|---|---|---|
| [0001](0001-license.md) | Apache License 2.0 | Accepted |
| [0002](0002-self-hosted-gpu-runner.md) | Self-hosted RTX 3060 Ti performance runner | Accepted |
| [0003](0003-build-configurations.md) | Build configurations, presets, and sanitizer policy | Accepted |
| [0004](0004-allocator-interface.md) | Allocator interface | Accepted |
| [0005](0005-atomics-in-core.md) | Atomics live in core; the logger thread lives in platform | Accepted |

## Template

```markdown
# NNNN. Title

- **Date:** YYYY-MM-DD
- **Status:** Proposed | Accepted | Superseded by [NNNN](NNNN-title.md)

## Context
What forces are at play: requirements, constraints, measurements.

## Decision
What we chose, stated so that it can be checked against the code.

## Alternatives considered
Each rejected option and why.

## Consequences
What becomes easier, what becomes harder, what must be revisited later.
```

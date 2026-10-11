# 0001. Apache License 2.0

- **Date:** 2026-10-11
- **Status:** Accepted

## Context

Nepheline is developed in a public GitHub repository from the first commit.
The engine implements a large number of published techniques (CLAUDE.md §8
cites the papers) and is intended to ship commercial games, so both the
project and its users need clarity on copyright and on patents. The licence
had to be chosen before any code was pushed (CLAUDE.md §7, §12 Phase 0).

## Decision

Nepheline is licensed under the **Apache License, Version 2.0**.

- `LICENSE` holds the verbatim licence text (taken from the GitHub licence API).
- `NOTICE` names the copyright holder and will list third-party notices as
  vendored components arrive.
- Every source file starts with `// SPDX-License-Identifier: Apache-2.0`
  (or the `#` comment form) instead of the long boilerplate header.
- Contributions are accepted under the same licence (inbound = outbound,
  Apache-2.0 §5); no separate CLA.
- Third-party code in `third_party/` keeps its own licence. Only permissive
  licences compatible with shipping a closed-source game are acceptable
  (MIT, BSD, zlib, Apache-2.0, MPL-2.0 file-level). Copyleft licences (GPL,
  LGPL static linking) are excluded; adding any dependency already requires an
  ADR (CLAUDE.md §3).

## Alternatives considered

- **MIT** — simplest and most common for game engines (Godot, raylib). Lacks
  an explicit patent grant, which matters for a project built on published
  research where contributors may hold patents.
- **zlib / BSD-3-Clause** — equally permissive, equally silent on patents;
  zlib's attribution terms are slightly unusual for a large project.
- **Proprietary (all rights reserved)** — keeps every option open but
  contradicts developing in public and forbids outside contributions. Can be
  revisited for specific game projects built on the engine, which are
  separate repositories.

## Consequences

- Users and contributors receive an express patent licence from every
  contributor, with the defensive termination clause of §3.
- Dependency choices are constrained to permissive licences; this is already
  the stated policy and is now enforceable by licence compatibility.
- Binary distributions must carry `LICENSE` and `NOTICE`; the packaging work
  in Phase 15 has to include them.

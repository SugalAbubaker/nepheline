## Summary

<!-- What changes and why. Link the design doc or ADR when one applies. -->

## Checklist (CLAUDE.md §1, §9)

- [ ] Tests added or updated; the full suite passes locally (`ctest --preset <toolchain>-debug`)
- [ ] Zero compiler warnings on every toolchain; zero Vulkan validation and sync-validation messages (GPU changes)
- [ ] `python scripts/format.py --check` passes
- [ ] Public APIs documented: contract, ownership, thread-safety
- [ ] Design doc (`docs/<module>.md`) and ADR (`docs/adr/`) updated where a decision was made
- [ ] Performance claims backed by `benchmarks/` results
- [ ] Renderer changes state their cost in milliseconds **and** DRAM bytes (CLAUDE.md §10.7)

# 0003. Build configurations, presets, and sanitizer policy

- **Date:** 2026-10-11
- **Status:** Accepted

## Context

CLAUDE.md §6 prescribes four build configurations — Debug, Development,
Profile, Shipping — with specific semantics for asserts, validation layers,
the profiler, sanitizers, and LTO, plus presets for four toolchains
(`win-msvc`, `win-clang`, `linux-gcc`, `linux-clang`) and a zero-warning
policy. CMake's stock configurations (Debug, Release, RelWithDebInfo,
MinSizeRel) do not match those semantics, and its default Debug flags on
MSVC (`/RTC1`, incremental linking) are incompatible with AddressSanitizer.

## Decision

1. **Custom configurations replace CMake's.** `cmake/NphConfigurations.cmake`
   sets `CMAKE_CONFIGURATION_TYPES` to exactly the four Nepheline
   configurations before `project()` and owns every per-configuration compiler
   and linker flag afterwards. Per-configuration preprocessor definitions
   (`NPH_CONFIG_*`, `NPH_ASSERTS_ENABLED`, `NPH_GPU_VALIDATION_ENABLED`,
   `NPH_PROFILER_ENABLED`) are attached to the `nph::options` interface
   target so code never tests `NDEBUG`.
2. **One configure per toolchain, Ninja Multi-Config.** Each toolchain has
   one configure preset (`build/<preset>/`) and the configuration is chosen
   at build/test time (`cmake --build --preset win-msvc-debug`). Sanitizer and
   LTO flags are therefore generator expressions on `$<CONFIG:...>`, not
   cache options. A single-config `linux-clang-tidy` preset exists only to
   produce a one-configuration `compile_commands.json` for clang-tidy.
3. **`nph::options` is the single source of compiler settings.** Warnings,
   warnings-as-errors, the C17 standard, configuration defines, and
   sanitizers live on this interface target; every module, tool, test, and
   sample links it. Third-party code (later phases) does not.
4. **Sanitizers in Debug only.** ASan on MSVC, GCC, and Clang; UBSan
   (`-fno-sanitize-recover=all`) on GCC/Clang. TSan is an option that
   excludes ASan and is used by dedicated CI jobs from Phase 4. On MSVC the
   ASan runtime DLL is copied next to the Debug binaries at configure time so
   CTest can launch them outside a developer prompt. **clang-cl ASan is off
   by default**: LLVM's runtime library shares its name with the MSVC CRT's
   fork of it, and `link.exe` resolves the wrong one; it is enabled once a CI
   run proves a working combination.
5. **LTO in Shipping via `CMAKE_INTERPROCEDURAL_OPTIMIZATION_SHIPPING`**
   (`/GL` + `/LTCG` on MSVC, `-flto` elsewhere), guarded by
   `check_ipo_supported`. MSVC warning C4702 (unreachable code) is suppressed
   in Shipping only: link-time code generation performs whole-program
   constant propagation and then reports code that is unreachable *given
   every call site in that executable*, e.g. the `return false` after a
   conditional `longjmp` in ntest when all callers pass `fatal = true`. It
   reports optimiser facts rather than source defects and fires at link time
   where it cannot be fixed locally; the other configurations keep it.
6. **Imported third-party targets** map Development and Profile to Release
   (then RelWithDebInfo) and Shipping to Release (then MinSizeRel) through
   `CMAKE_MAP_IMPORTED_CONFIG_*`.

## Alternatives considered

- **Reuse CMake's configuration names** (Development = RelWithDebInfo,
  Shipping = Release) — avoids custom configs but leaves the semantics
  implicit and makes `NDEBUG` the only switch for asserts, which §2 forbids.
- **Single-config Ninja, one build directory per configuration** — simpler
  generator expressions, but four configures per toolchain and four
  `compile_commands.json` files to keep straight.
- **Sanitizers as extra configurations** (`DebugASan`, `DebugTSan`) — makes
  the matrix explode and separates "Debug" from "Debug with ASan", which §6
  defines as the same thing.
- **Globally suppress C4702** — simpler, but the warning is useful at
  compile time in the non-LTO configurations and only misbehaves under LTCG.
- **`-fno-strict-aliasing` everywhere** — common in engines; declined for now.
  Code must be aliasing-correct (type punning through `memcpy`), which the
  sanitizers and `-Wstrict-aliasing` help enforce. Revisit if a measured
  optimisation needs it.

## Consequences

- IDE support is good: Visual Studio 2022 opens `CMakePresets.json`
  natively; VS Code CMake Tools and CLion understand multi-config presets.
- CI builds Shipping on at least one Windows and one Linux toolchain so LTO
  breakage is caught (the C4702 case was found this way locally).
- Adding a third-party library requires marking its include directories
  `SYSTEM` (so MSVC's `/external:*` and GCC/Clang system-header rules silence
  it) and never linking `nph::options` into it.
- clang-cl Debug builds in CI run without ASan until the runtime conflict is
  resolved; that gap is tracked in `docs/build.md`.

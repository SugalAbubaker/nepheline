# Building Nepheline

This document covers the build system: prerequisites, presets, configurations,
options, tests, formatting, static analysis, and CI. The decisions behind it are
in [ADR-0003](adr/0003-build-configurations.md).

## Prerequisites

| | Windows | Linux |
|---|---|---|
| Compiler | Visual Studio 2022 17.14+ with the "Desktop development with C++" workload (MSVC 14.44). Optional: "C++ Clang tools for Windows" for the `win-clang` preset | GCC ≥ 12 or Clang ≥ 16 |
| CMake | ≥ 3.25 (VS bundles 3.31) | ≥ 3.25 |
| Ninja | bundled with VS | `ninja-build` |
| Python | 3.9+ (format/tidy scripts only) | 3.9+ |
| Vulkan SDK | 1.3+ (from Phase 5) | 1.3+ (from Phase 5) |
| git-lfs | required from Phase 6 (binary assets) | same |

## Quick start

**Windows (PowerShell).** `scripts/dev_env.ps1` imports the MSVC x64
environment and puts the VS-bundled CMake, Ninja, and LLVM tools on `PATH`.
The "x64 Native Tools Command Prompt for VS 2022" works too, as does opening
the folder in Visual Studio, which reads `CMakePresets.json` directly.

```powershell
. .\scripts\dev_env.ps1
cmake --preset win-msvc                    # configure once per toolchain
cmake --build --preset win-msvc-debug      # Debug, Development, Profile, or Shipping
ctest --preset win-msvc-debug
```

**Linux.**

```sh
cmake --preset linux-gcc                   # or linux-clang
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Build output lands in `build/<preset>/bin/<Config>/` and `build/<preset>/lib/<Config>/`.

## Presets

One configure preset per toolchain; the configuration is selected by the build
and test presets. All use the **Ninja Multi-Config** generator.

| Configure preset | Compiler | Host |
|---|---|---|
| `win-msvc` | `cl` | Windows |
| `win-clang` | `clang-cl` | Windows |
| `linux-gcc` | `gcc` | Linux |
| `linux-clang` | `clang` | Linux |
| `linux-clang-tidy` | `clang`, single-config Development | Linux; produces `compile_commands.json` for clang-tidy only |

Build and test presets are named `<toolchain>-<configuration>` in lower case:
`win-msvc-debug`, `linux-clang-shipping`, and so on.

## Configurations (CLAUDE.md §6)

| | Debug | Development | Profile | Shipping |
|---|---|---|---|---|
| Optimisation | `/Od` · `-O0 -g3` | `/O2` · `-O2 -g` | `/O2` · `-O2 -g` | `/O2` + LTO · `-O3 -flto` |
| `NPH_ASSERTS_ENABLED` | 1 | 1 | — | — |
| `NPH_GPU_VALIDATION_ENABLED` | 1 | 1 | — | — |
| `NPH_PROFILER_ENABLED` | 1 | 1 | 1 | — |
| `NDEBUG` | — | — | defined | defined |
| Sanitizers | ASan (+UBSan on GCC/Clang) | — | — | — |
| Debug info | yes | yes | yes | — |

Code tests `NPH_CONFIG_DEBUG`, `NPH_CONFIG_DEVELOPMENT`, `NPH_CONFIG_PROFILE`,
`NPH_CONFIG_SHIPPING`, or the feature macros above — never `NDEBUG`, which only
exists so the C library's `assert()` follows the same policy.

All of this is defined in `cmake/NphConfigurations.cmake` and delivered to
targets through the `nph::options` interface library, which every Nepheline
target links.

## Options

| Option | Default | Effect |
|---|---|---|
| `NPH_BUILD_TESTS` | `ON` | Build ntest and the unit tests; register them with CTest |
| `NPH_WARNINGS_AS_ERRORS` | `ON` | `/WX` or `-Werror` |
| `NPH_ASAN` | `ON` (`OFF` for clang-cl) | AddressSanitizer in Debug |
| `NPH_UBSAN` | `ON` | UndefinedBehaviorSanitizer in Debug (GCC/Clang) |
| `NPH_TSAN` | `OFF` | ThreadSanitizer in Debug (GCC/Clang); excludes `NPH_ASAN` |

Pass them at configure time: `cmake --preset linux-clang -DNPH_ASAN=OFF -DNPH_TSAN=ON`.

### Warnings

`cmake/NphWarnings.cmake`: MSVC `/W4 /WX /permissive- /Zc:preprocessor
/external:env:INCLUDE /external:W0` plus C4062, C4242, C4254; GCC/Clang
`-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wdouble-promotion
-Wstrict-prototypes -Wmissing-prototypes -Wundef -Wvla -Wcast-qual -Wformat=2
-Wimplicit-fallthrough -Werror`. Third-party headers are added as `SYSTEM`
include directories so these do not apply to them. C4702 is suppressed in
Shipping only (see ADR-0003 §5).

### Sanitizers

Debug builds are instrumented through `nph::options` so every object file
and link step in the configuration agrees. UBSan runs with
`-fno-sanitize-recover=all`: undefined behaviour terminates the test instead
of logging. On MSVC the ASan runtime DLL is copied to `bin/Debug/` at
configure time so test executables start from any shell.

## Tests

Tests are built on [ntest](ntest.md) and registered with CTest, one CTest
entry per test executable (`nph_add_test` in `cmake/NphModule.cmake`). Each
run writes a JUnit report to `build/<preset>/test-results/<Config>/<name>.xml`.

```sh
ctest --preset linux-gcc-debug                      # everything
ctest --preset linux-gcc-debug -R core              # by CTest name
build/linux-gcc/bin/Debug/core_tests --filter=version.*   # by ntest filter
build/linux-gcc/bin/Debug/core_tests --help         # runner options
```

## Formatting and static analysis

```sh
python scripts/format.py --check     # what CI runs (clang-format 19.1.x, .clang-format)
python scripts/format.py --fix       # or: cmake --build --preset <p> --target nph-format
cmake --preset linux-clang-tidy && python scripts/tidy.py --build-dir build/linux-clang-tidy
```

`.clang-tidy` lists every disabled check with its reason. clang-tidy runs on
Linux in CI against the single-config preset; running it locally against an
MSVC compile database produces false positives from MSVC-only flags and is not
supported.

## Adding a module or a test

```cmake
# engine/<module>/CMakeLists.txt
nph_add_module(<module>
    SOURCES src/a.c src/b.c
    PUBLIC_DEPS nph::core)          # only modules to the left in CLAUDE.md §4

nph_add_test(<module>
    SOURCES tests/test_a.c tests/test_b.c
    DEPS nph::<module>)
```

`nph_add_module` creates `nph_<module>` with alias `nph::<module>`, exposes
`include/`, and links `nph::options`. `nph_add_test` creates
`<module>_tests` on `ntest::main` and registers it as CTest test `<module>`.

## Continuous integration

`.github/workflows/ci.yml` runs on every PR and push to `main`:

- **clang-format** (pinned 19.1.5 from PyPI, matching VS 2022's copy).
- **Build + test matrix:** windows-msvc, windows-clang, linux-gcc,
  linux-clang × Debug, Development; Shipping on windows-msvc and linux-gcc so
  LTO is exercised. JUnit results are uploaded as artifacts.
- **clang-tidy** on the `linux-clang-tidy` preset with LLVM 19.
- **CI** aggregates the above; it is the single required status check on `main`.

`.github/workflows/gpu-perf.yml` targets the self-hosted RTX 3060 Ti runner
and is manual-only until Phase 5 ([ADR-0002](adr/0002-self-hosted-gpu-runner.md)).

## Known gaps

- **clang-cl + ASan** is unverified and off by default (ADR-0003 §4).
- **Local Linux builds** are not possible on the primary development machine
  (no WSL); Linux is covered by CI only.
- **lavapipe GPU tests, MSVC `/analyze`, cppcheck, coverage (llvm-cov), fuzzing**
  are wired up in the phases that introduce the code they check (CLAUDE.md §9).

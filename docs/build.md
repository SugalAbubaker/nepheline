# Building Nepheline

This document covers the build system: prerequisites, presets, configurations,
options, tests, formatting, static analysis, and CI. The decisions behind it are
in [ADR-0003](adr/0003-build-configurations.md).

## Prerequisites

| | Windows | Linux |
|---|---|---|
| Compiler | Visual Studio 2022 17.14+ with the "Desktop development with C++" workload (MSVC 14.44). Optional: "C++ Clang tools for Windows" for the `win-clang` preset | GCC ≥ 12 or Clang ≥ 16, with the matching `g++`/`clang++` (the VMA implementation is one C++ translation unit; the presets set both compilers) |
| CMake | ≥ 3.25 (VS bundles 3.31) | ≥ 3.25 |
| Ninja | bundled with VS | `ninja-build` |
| Python | 3.9+ (format/tidy scripts only) | 3.9+ |
| Assembler | MASM (`ml64`, part of the MSVC toolset) for the fiber context switch | the GNU assembler via the C compiler driver |
| Window system | nothing extra | `libwayland-dev libxkbcommon-dev wayland-protocols xorg-dev` to build GLFW; `xvfb` to run the window tests headless |
| Submodules | `git submodule update --init --recursive` after cloning (GLFW, Vulkan-Headers, volk, and VMA live in `third_party/`) | same |
| Vulkan | the LunarG SDK (1.4.321 is the pinned reference; `scripts/install_vulkan_sdk.ps1 -Version 1.4.321.1` installs it unattended): `glslc` for the shader build, the validation layers for Debug and Development, and the loader | `glslc` and `libvulkan1`; `vulkan-validationlayers` for Debug and Development; `mesa-vulkan-drivers` for lavapipe, the software driver the GPU tests run on without a GPU |
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
ctest --preset linux-gcc-debug             # headless: xvfb-run -a ctest --preset linux-gcc-debug
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
| `NPH_BUILD_BENCHMARKS` | `ON` | Build nbench and the benchmarks; register a smoke run of each with CTest (label `benchmark`) |
| `NPH_BUILD_SAMPLES` | `ON` | Build `samples/`; register each as a CTest smoke run (label `sample`) |
| `NPH_BUILD_FUZZERS` | `OFF` | Build the libFuzzer targets in `tests/fuzz/` (Clang only; `-fsanitize=fuzzer`) |
| `NPH_WARNINGS_AS_ERRORS` | `ON` | `/WX` or `-Werror` |
| `NPH_ASAN` | `ON` (`OFF` for clang-cl) | AddressSanitizer in Debug |
| `NPH_UBSAN` | `ON` | UndefinedBehaviorSanitizer in Debug (GCC/Clang) |
| `NPH_TSAN` | `OFF` | ThreadSanitizer in Debug (GCC/Clang); excludes `NPH_ASAN` |
| `NPH_COVERAGE` | `OFF` | Instrument for llvm-cov source-based coverage (Clang only; combine with `-DNPH_ASAN=OFF -DNPH_UBSAN=OFF`) |
| `NPH_FIBERS_WIN32` | `OFF` | Windows only: build the fiber layer on the Win32 fiber API instead of the assembly context switch ([ADR-0010](adr/0010-fibers.md)) |

Pass them at configure time: `cmake --preset linux-clang -DNPH_ASAN=OFF -DNPH_TSAN=ON`.

### Shaders

GLSL sources under `shaders/` compile to SPIR-V at build time with `glslc`
(`cmake/NphShaders.cmake`): `nph_add_shaders(<target> SOURCES ...)` makes a
target whose outputs land in `build/<preset>/shaders/<name>.spv`, with
`#include` dependencies tracked through glslc's depfiles, and
`nph_target_use_shaders(<exe> <shader_target>)` gives an executable the
`NPH_SHADER_DIR` definition it loads them from plus the `shaders/shared/`
include path for the C/GLSL shared headers. Flags: `--target-env=vulkan1.3
-O -g -Werror`. If `glslc` is not on `PATH`, set `VULKAN_SDK`.

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
ctest --preset linux-gcc-debug -L unit              # unit tests only (skip benchmark smoke runs)
build/linux-gcc/bin/Debug/core_tests --filter=version.*   # by ntest filter
build/linux-gcc/bin/Debug/core_tests --help         # runner options
```

Benchmarks (`benchmarks/`, [docs/benchmarks.md](benchmarks.md)) build as
`<name>_bench` executables and run for real with
`build/<preset>/bin/Profile/core_alloc_bench --json=out.json`; CTest only
runs them for a few milliseconds each to make sure they still work.

### Coverage

Line coverage is measured with Clang's source-based instrumentation and
llvm-cov (`cmake/NphCoverage.cmake`). The targets from CLAUDE.md §9 (90 %
for core, math, and jobs; 80 % elsewhere) are enforced per module by
`scripts/coverage_check.py`, which reads the text of `llvm-cov report`.
Files under `tests/`, `tools/`, `benchmarks/`, and `samples/` are excluded.

```sh
cmake --preset linux-clang-tidy -DCMAKE_BUILD_TYPE=Debug -DNPH_COVERAGE=ON       -DNPH_ASAN=OFF -DNPH_UBSAN=OFF -DNPH_BUILD_BENCHMARKS=OFF
cmake --build build/linux-clang-tidy
LLVM_PROFILE_FILE=build/linux-clang-tidy/profiles/%p-%m.profraw       ctest --test-dir build/linux-clang-tidy -L unit
llvm-profdata merge -sparse build/linux-clang-tidy/profiles/*.profraw       -o build/linux-clang-tidy/coverage.profdata
llvm-cov report build/linux-clang-tidy/bin/core_tests       -instr-profile=build/linux-clang-tidy/coverage.profdata       -ignore-filename-regex='/(tests|tools|benchmarks|samples|third_party)/'       engine/core/ | tee core.txt
python scripts/coverage_check.py --module core core.txt
```

`llvm-cov show` on the same inputs prints annotated source for the lines
that were missed. The sanitizers are turned off because their runtime
distorts the counters and ASan does not combine with profiling on every
LLVM release.

## Formatting and static analysis

```sh
python scripts/format.py --check     # what CI runs (clang-format 19.1.x, .clang-format)
python scripts/format.py --fix       # or: cmake --build --preset <p> --target nph-format
cmake --preset linux-clang-tidy && python scripts/tidy.py --build-dir build/linux-clang-tidy
```

`scripts/format.py` looks for clang-format in `$CLANG_FORMAT`, then the
PyPI package, then `PATH`. Install the pinned package so local results
match CI exactly (other 19.x builds group `#include` blocks differently):

```sh
pip install clang-format==19.1.5
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
  LTO is exercised; one windows-msvc Development build with
  `-DNPH_FIBERS_WIN32=ON` so the fiber fallback keeps passing. Linux tests
  run under Xvfb so the window tests and the `platform_window` sample open
  a real window. JUnit results are uploaded as artifacts.
- **clang-tidy** on the `linux-clang-tidy` preset with LLVM 19.
- **coverage (llvm-cov)**: an instrumented Debug build with the sanitizers
  off runs the unit tests and checks every engine module against its
  coverage target (see [Coverage](#coverage)). The per-module reports are
  uploaded as the `coverage-reports` artifact.
- **tsan (jobs)**: a linux-clang Debug build with ThreadSanitizer in place
  of ASan and UBSan (`-DNPH_TSAN=ON -DNPH_ASAN=OFF -DNPH_UBSAN=OFF`) runs
  the job system's tests, stress suite included (CLAUDE.md §9 "TSan for
  jobs, audio, and streaming"). Locally: the same configure line, then
  `ctest --preset linux-clang-debug -R '^jobs$'`.
- **CI** aggregates the above; it is the single required status check on `main`.

`.github/workflows/benchmarks.yml` runs every benchmark nightly in the
Profile configuration on hosted Windows and Linux runners, checks them
against `benchmarks/budgets.txt`, and uploads the JSON results as artifacts.

`.github/workflows/fuzz.yml` builds the libFuzzer targets nightly with
Clang (Debug: ASan + UBSan) and runs each for two minutes from its seed
corpus; crashing inputs are uploaded as artifacts.

`.github/workflows/gpu-perf.yml` targets the self-hosted RTX 3060 Ti runner
([ADR-0002](adr/0002-self-hosted-gpu-runner.md)).

### GPU tests in CI

The gpu module's tests and samples need a Vulkan device. Linux jobs
install `mesa-vulkan-drivers` and point the loader at lavapipe
(`VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json`), install
`vulkan-validationlayers` so Debug and Development builds run with the
validation gate, and run CTest under Xvfb so the swapchain tests can open
a window. LeakSanitizer suppresses the loader's and lavapipe's
process-lifetime allocations (`tests/lsan.supp`). Windows runners have no
driver: the GPU tests report "skipped" and the samples exit with 77, which
CTest treats as skipped (`SKIP_RETURN_CODE`). Windows jobs install the
Vulkan SDK with `scripts/install_vulkan_sdk.ps1` (cached per version) for
`glslc`.

## Known gaps

- **clang-cl + ASan** is unverified and off by default (ADR-0003 §4).
- **Local Linux builds** are not possible on the primary development machine
  (no WSL); Linux is covered by CI only.
- **MSVC `/analyze`, cppcheck** are wired up in the phases that introduce
  the code they check (CLAUDE.md §9).
- **Windows CI has no Vulkan driver**, so the GPU tests only run on Linux
  (lavapipe) and on the self-hosted RTX 3060 Ti runner.

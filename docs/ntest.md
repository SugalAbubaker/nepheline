# ntest — the Nepheline test framework

`tools/ntest` is a small C17 unit-test framework with no dependencies beyond
the C library and the OS. It exists because CLAUDE.md §3 keeps third-party
code out of the engine and §9 asks for registration macros, fixtures,
assertions with diffs, CTest integration, and JUnit output.

## Writing tests

```c
#include <ntest/ntest.h>

NTEST(arena, alloc_is_aligned)
{
    NphArena arena = make_arena(1024);
    void *p = nph_arena_alloc(&arena, 24, 16);
    NTEST_ASSERT_NOT_NULL(p);
    NTEST_ASSERT_EQ_UINT((uintptr_t)p % 16, 0);
}

typedef struct PoolFixture { NphPool pool; } PoolFixture;

NTEST_SETUP(PoolFixture)    { nph_pool_init(&f->pool, 64, 32); }
NTEST_TEARDOWN(PoolFixture) { nph_pool_destroy(&f->pool); }

NTEST_F(PoolFixture, exhausts_then_refuses)
{
    for (int i = 0; i < 32; ++i) {
        NTEST_ASSERT_NOT_NULL(nph_pool_alloc(&f->pool));
    }
    NTEST_ASSERT_NULL(nph_pool_alloc(&f->pool));
}
```

- `NTEST(suite, name)` defines a test. `NTEST_F(Fixture, name)` defines a
  test whose suite is the fixture type; `f` points at zero-initialised
  fixture memory that `NTEST_SETUP` has run on. Both `NTEST_SETUP` and
  `NTEST_TEARDOWN` must be defined for the fixture earlier in the same file.
  Teardown runs even when the test aborts.
- Tests register themselves when the executable loads. Link `ntest::main`
  for `main()`, or call `ntest_main(argc, argv)` yourself.
- The suite name of a test may be any identifier; related tests share a suite.
- A test that does not touch its fixture writes `NTEST_UNUSED(f)` — the
  compiler flags unused parameters as errors.

### Assertions

`NTEST_ASSERT_*` aborts the test on failure. `NTEST_EXPECT_*` records the
failure, returns `false`, and continues.

| Family | Macros (`ASSERT_` shown; `EXPECT_` identical) | Notes |
|---|---|---|
| Boolean | `TRUE(x)`, `FALSE(x)` | |
| Signed integers | `EQ_INT`, `NE_INT`, `LT_INT`, `LE_INT`, `GT_INT`, `GE_INT` | via `intmax_t` |
| Unsigned integers | `EQ_UINT`, `NE_UINT`, `LT_UINT`, `LE_UINT`, `GT_UINT`, `GE_UINT` | via `uintmax_t`; use for anything ≥ 2⁶³ |
| Pointers | `EQ_PTR`, `NE_PTR`, `NULL(p)`, `NOT_NULL(p)` | |
| Strings | `EQ_STR`, `NE_STR` | NULL-safe; reports the first differing index |
| Memory | `EQ_MEM(a, b, n)` | reports the first differing byte with a hex window |
| Floats, absolute | `NEAR_F32(a, b, tol)`, `NEAR_F64(a, b, tol)` | `a == b` or `|a−b| ≤ tol`; NaN never passes |
| Floats, ULP | `ULP_F32(a, b, ulps)`, `ULP_F64(a, b, ulps)` | representable values apart; ±0 are 0 apart |
| Explicit | `NTEST_FAIL(fmt, ...)`, `NTEST_ADD_FAILURE(fmt, ...)` | fatal / non-fatal |
| Control | `NTEST_SKIP(fmt, ...)`, `NTEST_LOG(fmt, ...)` | skip is immediate; logs show for failures (or with `--verbose`) |

Every failure prints both operand expressions and their values:

```
[  FAILED  ] fail.eq_str (0.03 ms)
  tools/ntest/tests/test_ntest_failing.c:24: expected strings equal
      lhs: "alpha" = "alpha"
      rhs: "alpXa" = "alpXa"
      differ at index 3: 'h' (0x68) vs 'X' (0x58)
```

ULP tolerances are the right tool for math tests (CLAUDE.md §8.2):
`NTEST_ASSERT_ULP_F32(result, reference, 4)` says "within four representable
values", which scales with magnitude where an absolute epsilon does not.

## Running tests

```
core_tests [options]
  --list              List the selected tests (suite.name per line) and exit
  --filter=PATTERNS   Comma-separated globs ('*', '?') on suite.name; a pattern
                      without '.' also matches a suite; prefix '-' excludes.
                      e.g. --filter=math.*,-math.slow_*
  --junit=FILE        Write a JUnit XML report (parent directories are created)
  --repeat=N          Run the selection N times
  --shuffle[=SEED]    Random order; the seed is printed so a run can be reproduced
  --fail-fast         Stop after the first failing test
  --quiet             Only failures and the summary
  --verbose           Also print NTEST_LOG output of passing tests
  --color, --no-color Force colour (default: terminals only, honours NO_COLOR)
```

Exit codes: `0` all passed (skips allowed), `1` a test failed, `2` usage
error or unwritable report, `3` nothing matched the filter.

Default order is deterministic — by source file, then line — regardless of
link order. `--shuffle` exists to find hidden inter-test dependencies.

CMake's `nph_add_test` registers each executable with CTest and passes
`--junit=<build>/test-results/<Config>/<name>.xml`; CI uploads those files.

## Design

**Registration.** Each `NTEST` expands to a static `NtestCase` record and a
load-time initializer that links it into a global list: GCC/Clang use
`__attribute__((constructor))`; MSVC uses a function pointer placed in the
`.CRT$XCU` section with a `/include:` linker directive so `/OPT:REF` cannot
discard it. The runner sorts the list before running, so registration order
is irrelevant.

**Fatal assertions.** A fatal failure calls `longjmp` to a `jmp_buf` armed
by the runner around *setup + body*, and armed again around *teardown*, so
teardown always runs and a failure inside teardown cannot re-enter it. The
two guarded functions keep no locals, which avoids clobbered-variable
hazards. Check functions return `bool` so `EXPECT_*` can be used in
conditions.

**Current-test state is a single global.** Tests run strictly sequentially on
the calling thread; this keeps the assertion macros free of context
parameters and lets assertions be made from helper functions.

**Allocation.** ntest uses the C library allocator. It is a tool, not engine
code, and it is what tests the engine's allocators; CLAUDE.md §1.4 applies to
`engine/`.

**ULP distance.** IEEE-754 bit patterns are mapped to a monotonic unsigned
integer space (sign-magnitude → offset binary), so the distance is a
subtraction. The two zeros map to the same point; NaN yields the maximum
value and never passes.

**JUnit.** One `<testsuite>` per distinct suite name, in first-appearance
order; `<testcase>` carries `classname`, `name`, `file`, `line`, `time`, a
`<failure message="first line">full text</failure>` block, `<skipped
message=...>`, and `<system-out>` for logs. Text is XML-escaped; control
characters other than tab/LF/CR become `?`.

## Limitations

- No crash isolation: a segfault ends the executable. Sanitizers report the
  cause; CTest's `TIMEOUT` and failure status cover hangs and crashes. A
  subprocess-based death-test mode is a candidate for a later phase.
- Single-threaded by design. Tests that exercise threads must join them
  before asserting from the test thread.
- MSVC registration assumes x64 symbol naming (no leading underscore).

## How ntest is tested

- `tools/ntest/tests/test_ntest.c` — the passing form of every assertion,
  fixture lifecycle, and the internal helpers (glob, filter, ULP, XML
  escaping, ordering).
- `tools/ntest/tests/test_ntest_failing.c` — an executable whose tests
  deliberately fail. `run_check.cmake` runs it with different options and
  asserts on the exit code, output substrings (including the absence of
  output after a fatal assertion), and the JUnit file. These are the
  `ntest.cli.*` CTest entries.

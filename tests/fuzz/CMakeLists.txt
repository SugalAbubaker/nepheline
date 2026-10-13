# SPDX-License-Identifier: Apache-2.0
# libFuzzer targets for every parser in core (CLAUDE.md §8.8 "every parser
# is fuzzed", §9). Built only with -DNPH_BUILD_FUZZERS=ON under Clang; the
# nightly fuzz workflow runs each with its seed corpus (tests/fuzz/corpus).

nph_add_fuzzer(json SOURCES fuzz_json.c DEPS nph::core)
nph_add_fuzzer(bin SOURCES fuzz_bin.c DEPS nph::core)
nph_add_fuzzer(utf8 SOURCES fuzz_utf8.c DEPS nph::core)
nph_add_fuzzer(cvar SOURCES fuzz_cvar.c DEPS nph::core)

# The SPIR-V reflection parser (engine/gpu, CLAUDE.md §8.5).
nph_add_fuzzer(spirv SOURCES fuzz_spirv.c DEPS nph::gpu)

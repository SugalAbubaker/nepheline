// SPDX-License-Identifier: Apache-2.0
#pragma once

// Asset identifiers (CLAUDE.md §8.8 "Asset database: 128-bit GUIDs"). The
// identifier type, its text form, derivation from text, and the seeded
// generator are core's (core/guid.h), so that scene files can hold asset
// references; this header adds what needs the platform: a generator
// seeded from the clocks, for assets the editor creates.
//
// Thread-safety: NPH_THREADSAFE; a generator belongs to one thread at a
// time.

#include <nepheline/core/guid.h>

#ifdef __cplusplus
extern "C" {
#endif

// Seeds the generator from the wall clock, the cycle counter, the process
// id, and a process-wide serial, so that two generators created in the
// same nanosecond still diverge. For reproducible streams (tests) use
// nph_guid_generator_seed.
void nph_guid_generator_init(NphGuidGenerator *g);

#ifdef __cplusplus
}
#endif

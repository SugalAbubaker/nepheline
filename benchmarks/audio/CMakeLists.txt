# SPDX-License-Identifier: Apache-2.0
# The audio module (CLAUDE.md §8.10; docs/audio.md "Performance"): the block
# kernels per SIMD level, the resampler, filters, delays, and panning.
nph_add_benchmark(audio
    SOURCES bench_audio.c
    DEPS nph::audio)

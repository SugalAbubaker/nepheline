# SPDX-License-Identifier: Apache-2.0
# The audio module (CLAUDE.md §8.10; docs/audio.md; docs/adr/0051-audio-module.md):
# the DSP building blocks, the resampler, and the block kernels, and (later
# in Phase 12) the codecs, the mixer, spatialisation, streaming, and the
# output device. Depends on platform (threads, files, asynchronous I/O) and
# through it on math and core; never on asset, scene, or physics: the
# runtime connects them.

nph_add_module(audio
    SOURCES
        src/audio.c
        src/audio_internal.h
        src/delay.c
        src/filter.c
        src/kernels.c
        src/kernels_avx2.c
        src/kernels_impl.h
        src/kernels_sse4.c
        src/pan.c
        src/resampler.c
    PUBLIC_DEPS nph::platform
)
nph_set_avx2_sources(src/kernels_avx2.c)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(audio
    SOURCES
        tests/test_audio.c
        tests/test_audio_support.h
        tests/test_delay.c
        tests/test_filter.c
        tests/test_kernels.c
        tests/test_pan.c
        tests/test_resampler.c
    DEPS
        nph::audio
)
if(NPH_BUILD_TESTS)
    # White-box tests reach the kernel tables directly.
    target_include_directories(audio_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

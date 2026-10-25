# SPDX-License-Identifier: Apache-2.0
# The audio module (CLAUDE.md §8.10; docs/audio.md; docs/adr/0051-audio-module.md):
# the DSP building blocks, the resampler, the block kernels, the codecs (WAV,
# our ADPCM, our lossless codec), the effects, the mixer, and (later in Phase
# 12) spatialisation, streaming, and the output device. Depends on platform
# (threads, files, asynchronous I/O) and through it on math and core; never
# on asset, scene, or physics: the runtime connects them.

nph_add_module(audio
    SOURCES
        src/adpcm.c
        src/audio.c
        src/audio_internal.h
        src/delay.c
        src/dynamics.c
        src/echo.c
        src/eq.c
        src/filter.c
        src/kernels.c
        src/kernels_avx2.c
        src/kernels_impl.h
        src/kernels_sse4.c
        src/lossless.c
        src/mixer.c
        src/mixer_api.c
        src/mixer_internal.h
        src/mixer_render.c
        src/mixer_voice.c
        src/pan.c
        src/resampler.c
        src/spatial.c
        src/reverb.c
        src/wav.c
    PUBLIC_DEPS nph::platform
)
nph_set_avx2_sources(src/kernels_avx2.c)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(audio
    SOURCES
        tests/test_adpcm.c
        tests/test_audio.c
        tests/test_audio_support.h
        tests/test_delay.c
        tests/test_dynamics.c
        tests/test_echo.c
        tests/test_eq.c
        tests/test_filter.c
        tests/test_kernels.c
        tests/test_lossless.c
        tests/test_mixer.c
        tests/test_mixer_realtime.c
        tests/test_mixer_support.h
        tests/test_pan.c
        tests/test_resampler.c
        tests/test_spatial.c
        tests/test_reverb.c
        tests/test_wav.c
    DEPS
        nph::audio
)
if(NPH_BUILD_TESTS)
    # White-box tests reach the kernel tables directly.
    target_include_directories(audio_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

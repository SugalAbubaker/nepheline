# SPDX-License-Identifier: Apache-2.0
# The audio module (CLAUDE.md §8.10; docs/audio.md; docs/adr/0051-audio-module.md):
# the DSP building blocks, the resampler, the block kernels, the codecs (WAV,
# our ADPCM, our lossless codec), the effects, the mixer, spatialisation, the
# output device (over miniaudio), and streaming. Depends on platform
# (threads, files, asynchronous I/O) and through it on math and core; never
# on asset, scene, or physics: the runtime connects them.

nph_add_module(audio
    SOURCES
        src/adpcm.c
        src/audio.c
        src/audio_internal.h
        src/delay.c
        src/device.c
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
        src/mixer_effects.c
        src/mixer_internal.h
        src/mixer_render.c
        src/mixer_voice.c
        src/pan.c
        src/resampler.c
        src/spatial.c
        src/stream.c
        src/stream_internal.h
        src/reverb.c
        src/wav.c
    PUBLIC_DEPS nph::platform
)
nph_set_avx2_sources(src/kernels_avx2.c)

# miniaudio, for the output device only (CLAUDE.md §3; device.c is the one
# engine unit that includes it): its implementation is one unit at the
# library's own warning level, instrumented by the build's sanitizers like
# the code its thread calls into, never with the engine's warnings.
add_library(nph_audio_miniaudio STATIC src/miniaudio_impl.c)
target_link_libraries(nph_audio_miniaudio PRIVATE miniaudio)
target_compile_features(nph_audio_miniaudio PRIVATE c_std_17)
nph_apply_configuration_definitions(nph_audio_miniaudio)
nph_apply_sanitizers(nph_audio_miniaudio PRIVATE)
if(MSVC)
    target_compile_options(nph_audio_miniaudio PRIVATE /W0)
else()
    target_compile_options(nph_audio_miniaudio PRIVATE -w)
    # Strict C17 hides POSIX (clock_gettime, usleep, ...) behind the feature
    # macros, which miniaudio defines only in its decoders (compiled out),
    # and an undeclared function is an error to Clang, -w or not.
    target_compile_definitions(nph_audio_miniaudio PRIVATE _GNU_SOURCE)
    # The backends' libraries are loaded at run time (dlopen), so a machine
    # without PulseAudio or ALSA still runs, on the null device.
    find_package(Threads REQUIRED)
    target_link_libraries(nph_audio_miniaudio PUBLIC ${CMAKE_DL_LIBS} Threads::Threads m)
endif()
set_target_properties(nph_audio_miniaudio PROPERTIES FOLDER "third_party")
target_link_libraries(nph_audio PRIVATE nph_audio_miniaudio miniaudio)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(audio
    SOURCES
        tests/test_adpcm.c
        tests/test_audio.c
        tests/test_audio_support.h
        tests/test_delay.c
        tests/test_device.c
        tests/test_dynamics.c
        tests/test_echo.c
        tests/test_eq.c
        tests/test_filter.c
        tests/test_kernels.c
        tests/test_lossless.c
        tests/test_mixer.c
        tests/test_mixer_effects.c
        tests/test_mixer_realtime.c
        tests/test_mixer_spatial.c
        tests/test_mixer_support.h
        tests/test_pan.c
        tests/test_resampler.c
        tests/test_spatial.c
        tests/test_stream.c
        tests/test_reverb.c
        tests/test_wav.c
    DEPS
        nph::audio
)
if(NPH_BUILD_TESTS)
    # White-box tests reach the kernel tables directly.
    target_include_directories(audio_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

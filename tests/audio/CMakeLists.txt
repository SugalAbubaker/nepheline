# SPDX-License-Identifier: Apache-2.0
# Golden mixes (CLAUDE.md §8.10 "offline rendering of mixes, compared
# against reference buffers within tolerance"; docs/audio.md "Golden
# mixes"): the mixer's test scenes rendered offline and compared with
# approved baselines (tests/audio/baselines, Git LFS). CTest label "golden".
# Renders go to <build>/audio_golden/<Config>/, where
# scripts/audio_golden_approve.py finds them.

nph_add_test(audio_golden
    SOURCES
        test_audio_golden.c
    DEPS
        nph::audio
        nph::platform
)
if(NPH_BUILD_TESTS)
    target_compile_definitions(audio_golden_tests PRIVATE
        "NPH_AUDIO_GOLDEN_BASELINE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/baselines\""
        "NPH_AUDIO_GOLDEN_OUTPUT_DIR=\"${CMAKE_BINARY_DIR}/audio_golden/$<CONFIG>\"")
    if(MSVC)
        target_compile_definitions(audio_golden_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_tests_properties(audio_golden PROPERTIES LABELS "golden")
endif()

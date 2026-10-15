# SPDX-License-Identifier: Apache-2.0
# Golden-image tests (CLAUDE.md §9 "GPU golden-image tests"; docs/golden.md;
# docs/adr/0019-golden-image-tests.md): the reference renderer's test
# scenes rendered headlessly and compared with approved baselines
# (tests/golden/baselines, Git LFS) with LDR-FLIP (tools/golden). CTest
# label "golden". Rendered images and FLIP maps go to
# <build>/golden/<Config>/, where scripts/golden_approve.py finds them.

nph_add_test(golden
    SOURCES
        test_golden.c
        "${CMAKE_SOURCE_DIR}/samples/common/cooked_scene.c"
        "${CMAKE_SOURCE_DIR}/samples/common/cooked_scene.h"
    DEPS
        nph::render
        nph::asset
        nph::golden
)
if(NPH_BUILD_TESTS)
    target_include_directories(golden_tests PRIVATE
        "${CMAKE_SOURCE_DIR}/engine/gpu/tests"
        "${CMAKE_SOURCE_DIR}/samples/common")
    nph_target_use_shaders(golden_tests nph_shaders_render)
    target_compile_definitions(golden_tests PRIVATE
        "NPH_GOLDEN_BASELINE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/baselines\""
        "NPH_GOLDEN_OUTPUT_DIR=\"${CMAKE_BINARY_DIR}/golden/$<CONFIG>\"")
    if(MSVC)
        target_compile_definitions(golden_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(NPH_GPU_REQUIRE_VALIDATION)
        target_compile_definitions(golden_tests PRIVATE NPH_GPU_REQUIRE_VALIDATION=1)
    endif()
    # The cooked scene: pbr_viewer's content, cooked for these tests.
    if(TARGET nph_cook)
        set(golden_content "${CMAKE_SOURCE_DIR}/samples/pbr_viewer/content")
        set(golden_out "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
        add_custom_command(
            OUTPUT "${golden_out}/pbr_scene.npk"
            COMMAND nph_cook "${golden_content}/pbr_scene.gltf" --root "${golden_content}"
                    -o "${golden_out}/cooked" --pack "${golden_out}/pbr_scene.npk" --quiet
            DEPENDS nph_cook "${golden_content}/pbr_scene.gltf"
            COMMENT "Cooking the golden-image content"
            VERBATIM)
        add_custom_target(golden_content DEPENDS "${golden_out}/pbr_scene.npk")
        set_target_properties(golden_content PROPERTIES FOLDER "tests")
        add_dependencies(golden_tests golden_content)
        target_compile_definitions(golden_tests PRIVATE
            "NPH_GOLDEN_PACK=\"${golden_out}/pbr_scene.npk\"")
    endif()
    set_tests_properties(golden PROPERTIES LABELS "golden")
endif()

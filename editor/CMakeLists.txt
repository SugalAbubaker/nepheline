# The editor (CLAUDE.md §8.13; docs/editor.md; docs/adr/0025-editor-ui.md):
# an executable over the engine's shared library, as the player and the game
# modules use it, with its interface in Dear ImGui (third_party/cimgui, the
# only C++ in the editor and never in the runtime). The editor's code is a
# library so its tests drive the same code the executable runs.

add_library(nph_editor_core STATIC
    src/camera.c
    src/camera.h
    src/document.c
    src/edit.c
    src/edit.h
    src/editor.c
    src/editor.h
    src/editor_internal.h
    src/editor_log.c
    src/editor_log.h
    src/history.c
    src/history.h
    src/imgui_c.h
    src/panel_console.c
    src/panel_inspector.c
    src/panel_outliner.c
    src/panel_profiler.c
    src/panel_stats.c
    src/panel_viewport.c
    src/selection.c
    src/selection.h
    src/ui_platform.c
    src/ui_platform.h
    src/ui_renderer.c
    src/ui_renderer.h)
target_include_directories(nph_editor_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(nph_editor_core PUBLIC nph::options nph::shared nph_cimgui)
nph_target_use_shaders(nph_editor_core nph_shaders_editor)
if(MSVC)
    target_compile_definitions(nph_editor_core PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(nph_editor_core PROPERTIES FOLDER "editor")

add_executable(nph_editor src/main.c)
target_link_libraries(nph_editor PRIVATE nph_editor_core)
set_target_properties(nph_editor PROPERTIES FOLDER "editor")

if(NPH_BUILD_TESTS)
    # The executable itself for a few frames, windowed, with no layout file.
    # Skipped (77) on machines without a window system or a Vulkan device.
    add_test(NAME editor.smoke COMMAND nph_editor --frames=60 editor.layout=)
    set_tests_properties(editor.smoke PROPERTIES TIMEOUT 120 LABELS "sample"
                                                 SKIP_RETURN_CODE 77)
endif()

nph_add_test(editor
    SOURCES
        tests/test_camera.c
        tests/test_editor.c
        tests/test_editor_log.c
        tests/test_history.c
        tests/test_selection.c
        tests/test_ui_platform.c
    DEPS
        nph_editor_core
)
if(NPH_BUILD_TESTS)
    if(MSVC)
        target_compile_definitions(editor_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    if(NPH_GPU_REQUIRE_VALIDATION)
        target_compile_definitions(editor_tests PRIVATE NPH_GPU_REQUIRE_VALIDATION=1)
    endif()
    # The runtime tests' cooked showcase (engine/runtime/CMakeLists.txt).
    add_dependencies(editor_tests runtime_test_content)
    target_compile_definitions(editor_tests PRIVATE
        "NPH_TEST_PACK=\"${CMAKE_BINARY_DIR}/engine/runtime/test_content/$<CONFIG>/showcase.npk\"")
endif()

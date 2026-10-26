# SPDX-License-Identifier: Apache-2.0
# The asset cooker (CLAUDE.md §8.8 "Offline cooker"; docs/cook.md;
# docs/adr/0017-cooker.md): nph::cook, a library the tests and samples
# use, and nph_cook, the command-line tool. cgltf and stb_image are
# vendored submodules (third_party/) that only this directory may include
# (scripts/check_layering.py).

# The two libraries' implementation translation units, compiled with
# their own warning policy and without the engine's sanitizer flags, like
# the VMA unit in engine/gpu. Everything else in the cooker sees only
# their declarations, as SYSTEM headers.
add_library(nph_cook_thirdparty STATIC src/cgltf_impl.c src/stb_impl.c)
target_link_libraries(nph_cook_thirdparty PRIVATE cgltf stb)
target_compile_features(nph_cook_thirdparty PRIVATE c_std_17)
if(MSVC)
    target_compile_options(nph_cook_thirdparty PRIVATE /W0)
    target_compile_definitions(nph_cook_thirdparty PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(nph_cook_thirdparty PRIVATE -w)
endif()
nph_apply_configuration_definitions(nph_cook_thirdparty)
# cgltf and stb_image are the parsers the cooker's fuzzers exercise most:
# instrument them for libFuzzer's coverage like first-party code
# (cmake/NphSanitizers.cmake).
if(NPH_BUILD_FUZZERS AND CMAKE_C_COMPILER_ID STREQUAL "Clang"
   AND NOT CMAKE_C_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    target_compile_options(nph_cook_thirdparty PRIVATE -fsanitize=fuzzer-no-link)
endif()
set_target_properties(nph_cook_thirdparty PROPERTIES FOLDER "tools")

set(cook_sources
    src/bc_decode.c
    src/bc_encode.c
    src/cook.c
    src/cooker.c
    src/database.c
    src/dds.c
    src/gltf.c
    src/image.c
    src/mesh.c
    src/mesh_lod.c
    src/mesh_voxel.c
    src/mesh_meshlet.c
    src/mesh_optimize.c
    src/mesh_tangent.c
    src/polymesh.c
    src/sound.c
    src/specgloss.c
    src/texture.c
)
file(GLOB_RECURSE cook_headers CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")

# nph_add_cooklib(<target> <engine>...): the cooker library over <engine>,
# the engine's static modules or its shared library.
function(nph_add_cooklib target)
    add_library(${target} STATIC ${cook_sources} ${cook_headers})
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${target}
        PUBLIC nph::options ${ARGN}
        PRIVATE nph_cook_thirdparty cgltf stb)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    set_target_properties(${target} PROPERTIES FOLDER "tools")
endfunction()

# The asset runtime for the formats, the renderer for its meshlet builder
# (render/meshlet.h), which cooked and run-time meshes share, and the audio
# module for the WAV reader and the sound encoders (audio/lossless.h,
# audio/adpcm.h), which the runtime's decoders pair with.
nph_add_cooklib(nph_cooklib nph::asset nph::render nph::audio)
add_library(nph::cook ALIAS nph_cooklib)

# The editor's cooker (docs/adr/0026-editor-import.md): the same sources
# over the engine's shared library, because the editor runs one engine (one
# log, one job system), which a second, static copy of the modules the
# cooker uses would split in two.
if(NPH_BUILD_EDITOR)
    nph_add_cooklib(nph_cooklib_shared nph::shared)
    add_library(nph::cook_shared ALIAS nph_cooklib_shared)
endif()

add_executable(nph_cook src/main.c)
target_link_libraries(nph_cook PRIVATE nph::options nph::cook)
if(MSVC)
    target_compile_definitions(nph_cook PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
set_target_properties(nph_cook PROPERTIES FOLDER "tools")

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(cook
    SOURCES
        tests/test_bc.c
        tests/test_cook_support.h
        tests/test_cooker.c
        tests/test_database.c
        tests/test_dds.c
        tests/test_gltf.c
        tests/test_image.c
        tests/test_mesh.c
        tests/test_polymesh.c
        tests/test_sound.c
        tests/test_specgloss.c
        tests/test_texture.c
        tests/test_voxel.c
    DEPS
        nph::cook
)
if(NPH_BUILD_TESTS)
    if(MSVC)
        target_compile_definitions(cook_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    # The command-line tool is exercised end to end, and the fuzz seeds of
    # the cooker's inputs must import (scripts/fuzz_seeds_cook.py).
    add_dependencies(cook_tests nph_cook)
    target_compile_definitions(cook_tests PRIVATE
        "NPH_COOK_EXE=\"$<TARGET_FILE:nph_cook>\""
        "NPH_FUZZ_CORPUS_DIR=\"${CMAKE_SOURCE_DIR}/tests/fuzz/corpus\"")
endif()

# SPDX-License-Identifier: Apache-2.0
# Vendored third-party libraries (CLAUDE.md §3): each is a pinned git
# submodule, built as part of the tree, wrapped by exactly one engine module,
# and added as SYSTEM so the engine's warning policy never applies to it.
# `git submodule update --init` fetches them.

function(_nph_require_submodule dir marker)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${marker}")
        message(FATAL_ERROR
            "third_party/${dir} is empty. Run: git submodule update --init --recursive")
    endif()
endfunction()

_nph_require_submodule(glfw CMakeLists.txt)
_nph_require_submodule(vulkan_headers CMakeLists.txt)
_nph_require_submodule(volk CMakeLists.txt)
_nph_require_submodule(vma CMakeLists.txt)
_nph_require_submodule(cgltf cgltf.h)
_nph_require_submodule(stb stb_image.h)
_nph_require_submodule(miniaudio miniaudio.h)
if(NPH_BUILD_EDITOR)
    _nph_require_submodule(cimgui imgui/imgui.h) # cimgui's own submodule, Dear ImGui
endif()

# GLFW 3.4: window, input, monitors (wrapped by engine/platform, window.h).
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_LIBRARY_TYPE "STATIC" CACHE STRING "" FORCE)
add_subdirectory(glfw SYSTEM)
set_target_properties(glfw PROPERTIES FOLDER "third_party")
if(TARGET update_mappings)
    set_target_properties(update_mappings PROPERTIES FOLDER "third_party")
endif()

# Vulkan-Headers, pinned to the SDK release the engine is developed against
# (docs/adr/0014-gpu-module.md). The headers are the only part of the Vulkan
# SDK the engine code needs; the loader is found at run time by volk, and
# glslc is a build tool (cmake/NphShaders.cmake). Vendoring them means every
# machine compiles against the same API version regardless of which SDK or
# distribution package it has installed.
set(VULKAN_HEADERS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(vulkan_headers SYSTEM)

# volk: Vulkan function loading (wrapped by engine/gpu). VOLK_PULL_IN_VULKAN
# is off so volk compiles against the vendored headers above rather than
# whatever FindVulkan or $VULKAN_SDK points at.
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
set(VOLK_INSTALL OFF CACHE BOOL "" FORCE)
set(VOLK_HEADERS_ONLY OFF CACHE BOOL "" FORCE)
add_subdirectory(volk SYSTEM)
target_link_libraries(volk PUBLIC Vulkan::Headers)
set_target_properties(volk PROPERTIES FOLDER "third_party")

# Vulkan Memory Allocator: header-only interface; the implementation is
# compiled as one isolated C++ translation unit inside engine/gpu
# (src/gpu_vma.cpp), the only C++ in the runtime.
set(VMA_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(vma SYSTEM)

# cgltf 1.15: glTF 2.0 parsing, offline only (wrapped by tools/cook,
# src/gltf.c; docs/adr/0017-cooker.md). Header-only: the implementation is
# compiled in tools/cook/src/cgltf_impl.c with the library's own warning
# policy. Never linked into the runtime.
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/cgltf")

# miniaudio 0.11.25: the audio output device, and only the device
# (wrapped by engine/audio, src/device.c; CLAUDE.md §3: "Mixing, DSP, and
# spatialization are ours"; docs/adr/0056-audio-device-streaming.md). The
# implementation is one unit at the library's own warning level
# (engine/audio/src/miniaudio_impl.c). Its configuration lives on this
# interface, so every unit that includes miniaudio.h sees the same
# structures: no decoding, encoding, generation, resource manager, node
# graph, or engine; only WASAPI on Windows, ALSA and PulseAudio (PipeWire's
# too) on Linux, and the null device for headless runs.
add_library(miniaudio INTERFACE)
target_include_directories(miniaudio SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/miniaudio")
target_compile_definitions(miniaudio INTERFACE
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH
    MA_NO_ENGINE MA_ENABLE_ONLY_SPECIFIC_BACKENDS MA_ENABLE_NULL)
if(WIN32)
    target_compile_definitions(miniaudio INTERFACE MA_ENABLE_WASAPI)
else()
    target_compile_definitions(miniaudio INTERFACE MA_ENABLE_ALSA MA_ENABLE_PULSEAUDIO)
endif()

# stb_image 2.30 (stb at 2c980bb5): PNG, JPEG, TGA, BMP, and Radiance HDR
# decoding, offline only (wrapped by tools/cook, src/image.c). Header-only;
# the implementation is tools/cook/src/stb_impl.c.
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/stb")

# cimgui at its docking_inter branch (125f397e0982), over Dear ImGui
# 1.92.9b's docking branch (b48d1afb): the editor's UI, and only the
# editor's (CLAUDE.md §3: "C++ compiled only for the editor target. Never
# in the runtime"; docs/adr/0025-editor-ui.md). Built with ImGui's own
# configuration (cimconfig.h keeps its asserts) and without its Win32 and
# shell helpers: the editor's platform backend provides the clipboard.
# C callers see the C declarations of cimgui.h (CIMGUI_DEFINE_ENUMS_AND_STRUCTS).
if(NPH_BUILD_EDITOR)
    add_library(nph_cimgui STATIC
        cimgui/cimgui.cpp
        cimgui/imgui/imgui.cpp
        cimgui/imgui/imgui_draw.cpp
        cimgui/imgui/imgui_tables.cpp
        cimgui/imgui/imgui_widgets.cpp
        cimgui/imgui/imgui_demo.cpp)
    target_compile_features(nph_cimgui PRIVATE cxx_std_17)
    target_include_directories(nph_cimgui SYSTEM PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cimgui")
    target_include_directories(nph_cimgui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/cimgui/imgui")
    target_compile_definitions(nph_cimgui
        PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS=1 IMGUI_DISABLE_WIN32_FUNCTIONS
               IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS
        PRIVATE "IMGUI_USER_CONFIG=\"../cimconfig.h\""
        INTERFACE CIMGUI_DEFINE_ENUMS_AND_STRUCTS)
    if(MSVC)
        target_compile_options(nph_cimgui PRIVATE /W0)
    else()
        target_compile_options(nph_cimgui PRIVATE -w)
    endif()
    set_target_properties(nph_cimgui PROPERTIES FOLDER "third_party")
endif()

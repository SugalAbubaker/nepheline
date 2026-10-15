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

# stb_image 2.30 (stb at 2c980bb5): PNG, JPEG, TGA, BMP, and Radiance HDR
# decoding, offline only (wrapped by tools/cook, src/image.c). Header-only;
# the implementation is tools/cook/src/stb_impl.c.
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/stb")

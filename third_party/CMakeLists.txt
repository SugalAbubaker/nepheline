# SPDX-License-Identifier: Apache-2.0
# Vendored third-party libraries (CLAUDE.md §3): each is a pinned git
# submodule, built as part of the tree, wrapped by exactly one engine module,
# and added as SYSTEM so the engine's warning policy never applies to it.
# `git submodule update --init` fetches them.

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/glfw/CMakeLists.txt")
    message(FATAL_ERROR
        "third_party/glfw is empty. Run: git submodule update --init --recursive")
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

# SPDX-License-Identifier: Apache-2.0
# Sample applications: one small executable per feature (CLAUDE.md §4, §9).
# Each is also registered with CTest as a smoke run (label "sample").

nph_add_sample(core_hello
    SOURCES core_hello/main.c
    DEPS nph::core
    ARGS r.vsync=false sample.entities=3 game.player_name=Ada)

nph_add_sample(math_demo
    SOURCES math_demo/main.c
    DEPS nph::math)

# Opens a window and logs its events and the mapped actions for a few
# frames (the CI smoke run) or until closed (CLAUDE.md §12 Phase 3 exit).
nph_add_sample(platform_window
    SOURCES platform_window/main.c
    DEPS nph::platform
    ARGS --frames=30)

# Fork-join, parallel-for, an I/O job, and the overlapped frame pipeline
# on the job system (CLAUDE.md §8.4; docs/jobs.md).
nph_add_sample(jobs_demo
    SOURCES jobs_demo/main.c
    DEPS nph::jobs
    ARGS --frames=60)

# The GPU samples exit with 77 on a machine without a window system or a
# Vulkan device, which CTest reports as skipped (CLAUDE.md §12 Phase 5).
nph_add_sample(gpu_triangle
    SOURCES gpu_triangle/main.c
    DEPS nph::gpu
    ARGS --frames=60
    SKIP_RETURN_CODE 77)
nph_add_sample(gpu_mesh
    SOURCES gpu_mesh/main.c
    DEPS nph::gpu
    ARGS --frames=120
    SKIP_RETURN_CODE 77)
if(TARGET gpu_triangle)
    nph_target_use_shaders(gpu_triangle nph_shaders_samples)
    nph_target_use_shaders(gpu_mesh nph_shaders_samples)
endif()

# The Phase 6 sample (CLAUDE.md §12, "a glTF scene cooks and loads"): its
# content is cooked when it is built, as a game's would be, with nph_cook
# turning the glTF source into a pack; the program loads the pack through
# the VFS and the asset loader and draws the cooked streams as they are.
# It exits with 77 like the GPU samples without a window system or device.
if(TARGET nph_cook)
    set(asset_viewer_content "${CMAKE_CURRENT_SOURCE_DIR}/asset_viewer/content")
    set(asset_viewer_out "${CMAKE_CURRENT_BINARY_DIR}/asset_viewer/$<CONFIG>")
    add_custom_command(
        OUTPUT "${asset_viewer_out}/showcase.npk"
        COMMAND nph_cook "${asset_viewer_content}/showcase.gltf" --root "${asset_viewer_content}"
                -o "${asset_viewer_out}/cooked" --pack "${asset_viewer_out}/showcase.npk" --quiet
        DEPENDS nph_cook "${asset_viewer_content}/showcase.gltf"
        COMMENT "Cooking the asset_viewer content"
        VERBATIM)
    add_custom_target(asset_viewer_content DEPENDS "${asset_viewer_out}/showcase.npk")
    set_target_properties(asset_viewer_content PROPERTIES FOLDER "samples")
    nph_add_sample(asset_viewer
        SOURCES asset_viewer/main.c
        DEPS nph::gpu nph::asset
        ARGS --frames=120 "--capture=${asset_viewer_out}/capture.bmp"
        SKIP_RETURN_CODE 77)
    if(TARGET asset_viewer)
        add_dependencies(asset_viewer asset_viewer_content)
        target_compile_definitions(asset_viewer PRIVATE
            "NPH_SAMPLE_PACK=\"${asset_viewer_out}/showcase.npk\"")
        nph_target_use_shaders(asset_viewer nph_shaders_samples)
    endif()
endif()

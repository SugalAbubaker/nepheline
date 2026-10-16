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

# The Phase 7 sample (CLAUDE.md §12, "Renderer v1"): the forward PBR
# reference renderer drawing cooked content. pbr_scene.gltf exercises every
# material feature the renderer implements; it is cooked at build time like
# the asset_viewer's content, and loaded through samples/common/
# cooked_scene.c, the asset-to-renderer bridge the golden tests share.
if(TARGET nph_cook)
    set(pbr_viewer_content "${CMAKE_CURRENT_SOURCE_DIR}/pbr_viewer/content")
    set(pbr_viewer_out "${CMAKE_CURRENT_BINARY_DIR}/pbr_viewer/$<CONFIG>")
    add_custom_command(
        OUTPUT "${pbr_viewer_out}/pbr_scene.npk"
        COMMAND nph_cook "${pbr_viewer_content}/pbr_scene.gltf" --root "${pbr_viewer_content}"
                -o "${pbr_viewer_out}/cooked" --pack "${pbr_viewer_out}/pbr_scene.npk" --quiet
        DEPENDS nph_cook "${pbr_viewer_content}/pbr_scene.gltf"
        COMMENT "Cooking the pbr_viewer content"
        VERBATIM)
    add_custom_target(pbr_viewer_content DEPENDS "${pbr_viewer_out}/pbr_scene.npk")
    set_target_properties(pbr_viewer_content PROPERTIES FOLDER "samples")
    nph_add_sample(pbr_viewer
        SOURCES pbr_viewer/main.c common/cooked_scene.c common/cooked_scene.h common/capture.c
                common/capture.h
        DEPS nph::render nph::asset
        ARGS --frames=120 "--capture=${pbr_viewer_out}/capture.bmp"
             "--profile=${pbr_viewer_out}/gpu_profile.json"
        SKIP_RETURN_CODE 77)
    if(TARGET pbr_viewer)
        add_dependencies(pbr_viewer pbr_viewer_content)
        target_include_directories(pbr_viewer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
        target_compile_definitions(pbr_viewer PRIVATE
            "NPH_SAMPLE_PACK=\"${pbr_viewer_out}/pbr_scene.npk\"")
        nph_target_use_shaders(pbr_viewer nph_shaders_render)
    endif()
endif()

# The Phase 8 sample (CLAUDE.md §12, "Scene/ECS"): an orrery built as an ECS
# world, moved by systems on the scheduler, drawn by the reference
# renderer, and round-tripped through both scene formats before the first
# frame. The headless run exercises the world, the schedule, and the scene
# files on machines without a window system or a GPU.
nph_add_sample(scene_demo
    SOURCES scene_demo/main.c common/capture.c common/capture.h
    DEPS nph::scene nph::render
    ARGS --frames=120 "--capture=${CMAKE_CURRENT_BINARY_DIR}/scene_demo/capture.bmp"
         "--save=${CMAKE_CURRENT_BINARY_DIR}/scene_demo/orrery.scene"
    SKIP_RETURN_CODE 77)
if(TARGET scene_demo)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/scene_demo")
    target_include_directories(scene_demo PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/common")
    nph_target_use_shaders(scene_demo nph_shaders_render)
    if(NPH_BUILD_TESTS)
        add_test(NAME sample.scene_demo.headless
                 COMMAND scene_demo --headless --frames=120 --asteroids=2000)
        set_tests_properties(sample.scene_demo.headless PROPERTIES TIMEOUT 120 LABELS "sample")
    endif()
endif()

# The Phase 9 sample (CLAUDE.md §12, "Runtime + Editor v1"): a game module,
# run by nph_player (and the editor) on the asset viewer's content. Its smoke
# runs play 240 frames in a window (77 without one or without a device) and
# headless, where the simulation runs without a GPU.
if(TARGET asset_viewer_content AND NPH_BUILD_SAMPLES)
    nph_add_game_module(game_sandbox SOURCES game_sandbox/game.c)
    set_target_properties(game_sandbox PROPERTIES FOLDER "samples")
    add_dependencies(game_sandbox asset_viewer_content nph_player)
    if(NPH_BUILD_TESTS)
        add_test(NAME sample.game_sandbox
                 COMMAND nph_player "--game=$<TARGET_FILE:game_sandbox>"
                         "--pack=${asset_viewer_out}/showcase.npk" --frames=240)
        add_test(NAME sample.game_sandbox_headless
                 COMMAND nph_player "--game=$<TARGET_FILE:game_sandbox>"
                         "--pack=${asset_viewer_out}/showcase.npk" --frames=240 --headless
                         rt.lockstep=true)
        set_tests_properties(sample.game_sandbox sample.game_sandbox_headless PROPERTIES
                             TIMEOUT 120 LABELS "sample" SKIP_RETURN_CODE 77)
    endif()
endif()

# SPDX-License-Identifier: Apache-2.0
# The physics module (CLAUDE.md §8.9; docs/physics.md; docs/adr/0042-physics.md):
# collision geometry, convex hulls, the narrow phase, the dynamic AABB tree,
# and (later in Phase 11) the world, the solver, joints, queries, and the
# character controller. Depends on jobs (the parallel step) and through it on
# platform, math, and core; never on scene: the ECS bridge lives in runtime.

nph_add_module(physics
    SOURCES
        src/aabb_tree.c
        src/body.c
        src/collide.c
        src/collide_hull.c
        src/collide_mesh.c
        src/contact.c
        src/geometry.c
        src/gjk.c
        src/hull.c
        src/island.c
        src/joint.c
        src/joint_solver.c
        src/mesh.c
        src/physics_internal.h
        src/shape.c
        src/solver.c
        src/solver_avx2.c
        src/solver_impl.h
        src/solver_sse4.c
        src/world.c
        src/world_internal.h
    PUBLIC_DEPS nph::jobs
)
nph_set_avx2_sources(src/solver_avx2.c)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

nph_add_test(physics
    SOURCES
        tests/test_aabb_tree.c
        tests/test_collide.c
        tests/test_collide_mesh.c
        tests/test_geometry.c
        tests/test_gjk.c
        tests/test_hull.c
        tests/test_joint.c
        tests/test_mesh.c
        tests/test_physics_support.h
        tests/test_ragdoll.c
        tests/test_solvers.c
        tests/test_world.c
        tests/test_world_support.h
    DEPS
        nph::physics
)
if(NPH_BUILD_TESTS)
    # White-box tests read the module's internal structures (the box view).
    target_include_directories(physics_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()

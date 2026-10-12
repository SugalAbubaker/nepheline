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

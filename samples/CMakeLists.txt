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

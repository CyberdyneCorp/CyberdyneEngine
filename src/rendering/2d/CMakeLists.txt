# src/rendering/2d/ — Cyberdyne's 2D pipeline: primitives, sorting, batching, tilemaps, 2D lights
# and shadows, the screen-space distance field, and `Camera2D`. M8.b task 9.5.
#
# WHAT IS HERE AND WHERE THE DEVICE IS. `rendering-2d`: "2D rendering SHALL use the same RHI, render
# graph, shader system, and material model as 3D, but a distinct pipeline optimised for sorted,
# batched quad and mesh submission." This module is the CPU half of that pipeline — the sort key, the
# batch decision, the instance data, the chunk rebuild, the shadow geometry and the distance field —
# and it names no device, no command buffer and no pipeline object. That is the same division
# src/rendering/culling/ draws: it produces a visible set, and forward/ draws it.
#
# The consequence, and it is the point: every requirement in `rendering-2d` that this module covers
# is testable without a GPU, and its suite runs on a machine with no display.
#
# LAYER. `rendering` (4), a spelling of `scene`. Its dependencies are cy::core alone — not even the
# render server, because a sort key and a chunk rebuild need none of it.
cy_add_module(
    NAME cy_rendering_2d
    LAYER rendering
    ALIAS cy::rendering-2d
    TYPE STATIC
    SOURCES
        src/lighting.cpp
        src/sprite.cpp
        src/tilemap.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

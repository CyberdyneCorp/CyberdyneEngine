# src/rendering/gi_streaming/ — layer 4: the GI's scene content streamed with world cells. Issue #35.
#
# `rendering-global-illumination` ingests and evicts its scene per world cell, and
# `world-partition-and-streaming` publishes cell lifecycle events for subsystems to consume at
# defined points. This module is the one place the two meet, for the reason src/foliage/ and
# src/water/ each consume `world::CellEventQueue` in a module of their own: cy::rendering-gi names no
# world type, and cy::world names no renderer.
#
# It runs on the host and calls only `gi::IlluminationSystem`'s cell, field and volume calls; the
# device copies (cy::rendering-gi-gpu) follow by what changed, so it needs no device to be tested.
cy_add_module(
    NAME cy_rendering_gi_streaming
    LAYER rendering
    ALIAS cy::rendering-gi-streaming
    TYPE STATIC
    SOURCES src/gi_streaming.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-gi
                        cy::rendering-raytracing cy::world)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

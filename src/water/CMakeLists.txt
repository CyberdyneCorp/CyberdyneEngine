# src/water/ — CyberWater: oceans, lakes, rivers and pools as ONE water body abstraction over
# several simulation backends. M10 task 2.3, and `water` reaching Working.
#
# ================================================================================================
# THE DEPENDENCY LIST IS THE SPECIFICATION'S ACYCLICITY REQUIREMENT
# ================================================================================================
#
# `water` — "Shoreline": "Shoreline computation SHALL be OWNED BY WATER, which reads terrain and
# writes wetness, water distance, and water depth into environment fields. Terrain SHALL NOT query
# the water system directly; it SHALL read those fields. This dependency direction SHALL be
# maintained so that terrain and water do not become mutually dependent."
#
# So: cy::environment is here and cy::terrain is NOT, and cannot be — src/terrain/ does not exist on
# this tree and this module would still not name it if it did. What water needs from terrain is one
# number, the height of the bed under a point, and it takes it through `BedHeightSource` in
# shoreline.h: a callback the application installs, which a terrain row satisfies without this
# module ever naming it. The direction is one way at the link graph, which is the level at which a
# knot is actually prevented rather than discouraged.
#
# NO RENDERER, NO DEVICE, NO PHYSICS BACKEND. `water` requires a water surface closure in the
# material system, underwater rendering, and buoyancy "through the physics interface" — and each of
# those is a CONSUMER of what this module computes, not a caller into it. What lives here is the
# shared definition every one of them evaluates: `evaluate_displacement()` for the surface,
# `water_transmittance()` for the column, `compute_buoyancy()` for the forces. A renderer that
# linked this module would get the same numbers the physics query gets, which is the displacement
# contract's whole content; a module that linked the renderer could not be evaluated in a cook.
#
# LAYER. `scene` (4): the rung src/world/, src/environment/, src/gameplay/, src/navigation/ and
# src/camera/ all chose, and for the reason each of them gives — there is no `water` rung in
# `engine-architecture`'s table and inventing one would be a change to cmake/module.cmake that
# src/gameplay/CMakeLists.txt already declined to make. It is read after src/environment/, whose
# fields this module PRODUCES, and before every consumer of those fields.
#
# NO OPTION. `water` gives no removal switch, and the three rows that read its fields — terrain
# materials, foliage and audio — read them through the substrate rather than through this target,
# so there is no build in which removing it is coherent while those exist. cmake/features.cmake is
# unchanged by this module, deliberately.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_water
    LAYER scene
    ALIAS cy::water
    TYPE STATIC
    SOURCES
        src/body.cpp
        src/buoyancy.cpp
        src/displacement.cpp
        src/foam.cpp
        src/ocean.cpp
        src/query.cpp
        src/river.cpp
        src/shading.cpp
        src/shoreline.cpp
        src/streaming.cpp
        src/system.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::environment cy::world)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

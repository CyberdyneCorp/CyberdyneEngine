# src/terrain/ — CyberTerrain: the ground surface as a streamed hierarchical dataset. M10 section 2
# (tasks 2.1 and 2.2), and `terrain` reaching Working.
#
# ================================================================================================
# TWO TARGETS, AND THE LINE BETWEEN THEM IS THE RENDERER
# ================================================================================================
#
# cy::terrain is the runtime and the query side: tiles and their hierarchy, the representation
# interface and the queries every consumer uses, material layers and the runtime virtual-texture
# producer, deformation and its deltas, meshing, collision, the navigation contribution, and the
# modifier stack. It names no device, no pipeline and no material compiler.
#
# cy::terrain-cook is the half that names the renderer: a meshed tile becoming virtual geometry
# clusters through `rendering::vg::build_geometry()`, and the terrain surface material authored as
# an ordinary `rendering::material::MaterialGraph`. src/ml/ splits `cy::ml-cook` from `cy::ml` for
# the same reason and states it the same way: a dedicated server that meshes terrain for collision,
# and a cooker that never opens a device, should not link a cluster builder and a material compiler
# between them.
#
# The split is also what makes the specification's negative claim checkable. "There SHALL NOT be a
# terrain-specific renderer, a terrain-specific level-of-detail morphing scheme, or a
# terrain-specific shadow path" — the only file in this module that may contain one is src/cook.cpp,
# and src/cook.cpp is two calls into the engine's own builders.
#
# ================================================================================================
# LAYER, AND WHAT IS DELIBERATELY NOT A DEPENDENCY
# ================================================================================================
#
# LAYER. `scene` (4): the rung src/world/, src/gameplay/, src/navigation/, src/environment/ and
# src/camera/ all chose, for the reason each of them gives — there is no `terrain` rung in
# `engine-architecture`'s table and inventing one would be a change to cmake/module.cmake that
# src/gameplay/CMakeLists.txt already declined to make.
#
# WHY cy::environment IS A DEPENDENCY AND THE DIRECTION IS ONE WAY. `environment-fields` specifies
# fields BENEATH terrain, "not owned by any of them". Terrain READS moisture, wetness, snow depth,
# water distance, depth and flow through `environment::FieldReader`, and PRODUCES exactly one
# standard field — `soil` — through `FieldRegistry::claim()`, which refuses a second producer and
# names both. Nothing under src/environment/ names `cy/terrain/`, and that is what keeps the
# substrate free of terrain's opinion.
#
# WHY cy::navigation AND cy::servers-physics ARE DEPENDENCIES, AND WHY NEITHER IS CALLED. Terrain
# "SHALL contribute a navigation surface to `navigation`" and "SHALL produce a collision
# representation"; it produces a `navigation::NavSourceGeometry` and a
# `physics::HeightFieldDescription` — the exact inputs those systems already take from everything
# else — and hands them to the caller. It creates no body and builds no navmesh, because a
# subsystem that reached into physics during streaming is the re-entrancy
# `world-partition-and-streaming` forbids in as many words.
#
# NO RHI, NO RENDER GRAPH, NO DEVICE in cy::terrain. A terrain tile becomes a mesh here and becomes
# clusters in the cook target.
#
# NO OPTION. `terrain` gives no removal switch, and unlike CY_UI or CY_VFX there is nothing
# coherent to remove: M10's foliage, water and PCG rows all stand on this module, and a world
# without terrain is not a configuration this engine offers. cmake/features.cmake is therefore
# unchanged by this module, deliberately.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_terrain
    LAYER scene
    ALIAS cy::terrain
    TYPE STATIC
    SOURCES
        src/collision.cpp
        src/deform.cpp
        src/material.cpp
        src/meshing.cpp
        src/stack.cpp
        src/surface.cpp
        src/system.cpp
        src/tile.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::environment cy::navigation cy::servers-physics cy::world)

cy_add_module(
    NAME cy_terrain_cook
    LAYER rendering
    ALIAS cy::terrain-cook
    TYPE STATIC
    SOURCES src/cook.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values cy::terrain
                        cy::rendering-material cy::rendering-virtual-geometry)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

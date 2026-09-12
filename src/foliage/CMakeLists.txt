# src/foliage/ — CyberFoliage: vegetation as a GPU-instance ecosystem, with entities as the
# exception rather than the rule. M10 section 2 (task 2.4), and `foliage` reaching Working.
#
# ================================================================================================
# TWO TARGETS, AND THE LINE BETWEEN THEM IS THE RENDERER
# ================================================================================================
#
# cy::foliage is the runtime and query side: species declarations, the sixteen-byte instance and its
# clusters, deterministic placement against terrain and the environment fields, exceptions and their
# re-resolution, promotion to entities and back, ground cover as patch descriptions, wind response,
# the interaction field, the budget, streaming and the diagnostics. It names no device, no pipeline,
# no material compiler and no `rendering::` type at all.
#
# cy::foliage-render is src/cook.cpp, and it is three things and nothing else: the one mapping from
# `FoliageSurface` to `rendering::vg::SurfaceClass`, the publication of clusters as
# `rendering::vg::GeometryInstance` — the same record a static mesh publishes — and the adapter that
# hands foliage's priced ladder to the renderer's budget arbiter. src/terrain/'s cy::terrain-cook
# and src/ml/'s cy::ml-cook are the precedents and state the split the same way.
#
# The split is also what makes the specification's own claim checkable. "Foliage SHALL publish into
# the GPU scene as instances ... and SHALL be culled, shaded, and shadowed BY THE SAME PASSES as
# other geometry" — the only file in this module that may contain a pass is src/cook.cpp, and
# src/cook.cpp produces one engine-standard instance record and declares one ladder.
#
# ================================================================================================
# LAYER, AND WHAT IS DELIBERATELY A DEPENDENCY
# ================================================================================================
#
# LAYER. `scene` (4): the rung src/world/, src/environment/, src/water/, src/terrain/,
# src/gameplay/, src/navigation/ and src/camera/ all chose, for the reason each of them gives —
# there is no `foliage` rung in `engine-architecture`'s table and inventing one would be a change to
# cmake/module.cmake that src/gameplay/CMakeLists.txt already declined to make.
#
# WHY cy::terrain IS A DEPENDENCY. `foliage` requires placement "evaluated against environment
# fields AND TERRAIN: biome, slope, altitude, moisture, temperature, soil, sun exposure, water
# distance, noise, roads, and exclusion zones". Placement calls `terrain::TerrainQuery::sample()`,
# which answers for any representation — so a world with mesh cliffs and arches places on the
# surface that is actually there, and a hole is not a surface, which is what stops a tree being
# planted in a cave mouth. Nothing under src/terrain/ names `cy/foliage/`, and that direction is
# what keeps terrain free of foliage's opinion.
#
# WHY cy::environment IS A DEPENDENCY AND THE DIRECTION IS ONE WAY. Foliage READS moisture,
# temperature, biome, soil, water distance, human exclusion, burn state, snow depth, wetness and
# wind, and PRODUCES exactly one field — `vegetation` — through `FieldRegistry::claim()`, which
# refuses a second producer and names both.
#
# WHY cy::ecs IS A DEPENDENCY AND NOTHING IS CREATED. Promotion produces a `PromotionOrder` and
# binds an `ecs::Entity` the CALLER created; there is no `ecs::World&` anywhere in this module. A
# subsystem that reached into the ECS while a cell was streaming is the re-entrancy
# `world-partition-and-streaming` forbids, and src/terrain/ declines the same reach into physics and
# navigation for the same reason. The dependency is one 8-byte handle type.
#
# NO RHI, NO RENDER GRAPH, NO DEVICE in cy::foliage.
#
# NO OPTION. `foliage` gives no removal switch, and unlike CY_UI or CY_VFX there is nothing coherent
# to remove: M10's own artefact is a populated world, and a build without foliage is not a
# configuration this engine offers. cmake/features.cmake is therefore unchanged by this module,
# deliberately.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_foliage
    LAYER scene
    ALIAS cy::foliage
    TYPE STATIC
    SOURCES
        src/budget.cpp
        src/diagnostics.cpp
        src/exceptions.cpp
        src/grass.cpp
        src/instance.cpp
        src/interaction.cpp
        src/placement.cpp
        src/promotion.cpp
        src/species.cpp
        src/streaming.cpp
        src/system.cpp
        src/wind.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory cy::ecs
                        cy::environment cy::terrain cy::world)

cy_add_module(
    NAME cy_foliage_render
    LAYER rendering
    ALIAS cy::foliage-render
    TYPE STATIC
    SOURCES src/cook.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::foliage
                        cy::rendering-arbiter cy::rendering-virtual-geometry)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

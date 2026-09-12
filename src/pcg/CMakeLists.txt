# src/pcg/ — CyberPCG: deterministic, region-based procedural generation. M10 section 4, and
# `procedural-content-generation` reaching Working.
#
# ================================================================================================
# TWO TARGETS, AND THE SPLIT IS THE SPECIFICATION'S OWN DIRECTION OF DEPENDENCY
# ================================================================================================
#
# `cy::pcg` is the generator: typed datasets, the graph compiler, the evaluator, regions and their
# invalidation, caching, identity, overrides and provenance. It depends on `cy::environment`,
# because "Generators SHALL be able to READ any environment field ... as first-class inputs" and
# because a generator that WRITES one does it through the substrate's producer token rather than
# beside it. It depends on `cy::world` for the coordinate types a field is addressed by. It depends
# on NOTHING ELSE above the core, and the omissions are the requirement:
#
#   * NO cy::foliage and NO cy::terrain. "Generators SHALL produce results through OUTPUT ADAPTERS,
#     and the adapter SHALL determine the representation." An adapter is an extension point, so the
#     generator must not name the representations it can produce into — and a cook, a dedicated
#     server or a unit test that compiles a graph would otherwise link a renderer-facing vegetation
#     module it has no use for.
#   * NO cy::ecs AND NO ENTITY TYPE ANYWHERE. "A procedural result SHALL NOT be an entity by
#     default. A generator producing ten million trees SHALL produce a foliage population, not ten
#     million spawn operations." The strongest form of that sentence is a module that cannot spell
#     `Entity`, and cy::pcg is it. Note that cy::pcg-adapters below CAN reach one, through
#     cy::foliage's own dependency on cy::ecs for promotion — so the claim there is behavioural
#     rather than structural, and src/pcg/tests/test_adapters.cpp measures it as such.
#   * NO PHYSICS. "Build-time generation SHALL NOT require a running physics world in order to
#     perform geometric queries; canonical geometric representations SHALL be used." `SpatialQuery`
#     in execute.h is that interface, and the bridge to a real surface is in the second target.
#   * NO RHI, NO RENDER GRAPH, NO DEVICE.
#
# `cy::pcg-adapters` is where a generated result becomes a representation: the foliage adapter, the
# terrain stamp adapter, and the terrain-backed spatial query. It is what may name `cy::foliage` and
# `cy::terrain`. src/terrain/CMakeLists.txt splits `cy::terrain-cook` off for the same reason and
# src/foliage/CMakeLists.txt splits `cy::foliage-render`.
#
# LAYER. `scene` (4): the rung src/world/, src/environment/, src/terrain/, src/foliage/,
# src/weather/, src/gameplay/, src/navigation/ and src/camera/ all chose, and for the same reason
# each of them gives — there is no `pcg` rung in `engine-architecture`'s table and inventing one
# would be a change to cmake/module.cmake that src/gameplay/CMakeLists.txt already declined to make.
#
# NO OPTION. `procedural-content-generation` gives no removal switch, and there is nothing coherent
# to remove: M10's own artefact is a procedurally populated world. cmake/features.cmake is therefore
# unchanged by this module, deliberately.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_pcg
    LAYER scene
    ALIAS cy::pcg
    TYPE STATIC
    SOURCES
        src/adapters.cpp
        src/cache.cpp
        src/dataset.cpp
        src/diagnostics.cpp
        src/execute.cpp
        src/graph.cpp
        src/identity.cpp
        src/invalidation.cpp
        src/ops.cpp
        src/overrides.cpp
        src/program.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::environment cy::world)

# The representations. Read after cy::pcg because it consumes it, and it is the ONLY target in this
# directory that may name a representation module.
cy_add_module(
    NAME cy_pcg_adapters
    LAYER scene
    ALIAS cy::pcg-adapters
    TYPE STATIC
    SOURCES src/foliage_adapter.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::foliage cy::pcg cy::terrain)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# src/world/ — CyberWorld: the spatial and persistence layer above the ECS. Section 3 of the M6
# tasks, and `world-partition-and-streaming` reaching Working.
#
# ================================================================================================
# THE NAME IS THE FIRST REQUIREMENT
# ================================================================================================
#
# `world-partition-and-streaming`, "The persistent world is distinct from the ECS world": the engine
# "SHALL NOT name both 'world' in its API". `cy::ecs::World` is the ECS runtime container and
# nothing else is ever called one. What lives here is addressed as `WorldStreaming`,
# `WorldPartition`, `PartitionConfig`, `CookedCell`, `LayerTable` and `PersistenceOverlay` — the
# directory is `world/` and the namespace is `cy::world`, and no type in it is `World`.
#
# ONE DIRECTION ONLY. The persistent world PUBLISHES entities into the ECS world; no ECS system is
# required to know this layer exists. That is why `cy::ecs` is a dependency here and nothing under
# src/ecs/ names `cy/world/`.
#
# LAYER. `scene` (4): above cy::ecs (1) and the servers (2), below cy::runtime (5). There is no
# `world` layer in `engine-architecture`'s table, and inventing one would be a change to
# cmake/module.cmake — which src/gameplay/CMakeLists.txt already declined to make for the same
# reason, and for the same rung. `scene` is the correct existing one: it is where the modules that
# compose ECS data into game-facing structure sit.
#
# WHAT IS NOT HERE, AND WHY
#
#   No file I/O and no save format. `save-and-persistence` owns the overlay's ENCODING,
#   journalling, atomicity, incremental writing, migration and storage backends. This module owns
#   the MODEL the world maintains — which is the division `world-partition-and-streaming` states in
#   its own "Persistence overlay" requirement, and the reason overlay.h opens a file nowhere.
#
#   No renderer, no physics backend, no navigation. Cells carry PAYLOADS for those subsystems and
#   publish structured EVENTS they consume at their own defined points; the world never calls into
#   them. "Streaming SHALL NOT invoke arbitrary subsystem callbacks during activation, since
#   re-entrant callbacks during streaming are a reliable source of ordering bugs" — a dependency on
#   any of those here is what would make such a callback possible.
#
#   No allocator. Every container takes the allocator its owner was handed, so a streaming world's
#   memory is attributable to `MemoryDomain::World` and visible to the pressure system that has to
#   evict it.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_world
    LAYER scene
    ALIAS cy::world
    TYPE STATIC
    SOURCES
        src/activation.cpp
        src/cell.cpp
        src/coordinates.cpp
        src/hlod.cpp
        src/layers.cpp
        src/overlay.cpp
        src/partition.cpp
        src/sources.cpp
        src/streaming.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values cy::ecs
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# src/ecs/ — layer 1: entities, components, archetypes, queries, systems, and the deferred
# structural change that makes parallel systems safe.
#
# Section 2 of tasks.md. Governed by `ecs-core`, which reaches Working this milestone.
#
# WHAT BELONGS HERE. The world and its entity table; component registration through
# cy::core-reflect; archetypes and their chunks over cy::core-memory; queries and their cached
# archetype lists; systems, stages and the access declarations they are scheduled from; deferred
# structural change; resources and singletons; change detection; entity relationships; world
# snapshots; multiple worlds.
#
# WHAT DOES NOT BELONG HERE. Two things, and both are load-bearing.
#
#   No allocator (design.md §1). Archetype storage is <cy/core/memory/chunk_storage.h>. An ECS that
#   allocated outside M1's budget tree would be invisible to the memory-pressure system that has to
#   evict it at M6, which is exactly the subsystem M6's residency policy holds allocations from.
#
#   No Node (design.md §3). The façade is src/scene/ at layer 4. Not every entity has a node, and
#   this layer must never assume one does — an #include of <cy/scene/...> from here fails the build.
#
# THE DEPENDENCIES ARE THE DESIGN. cy::core-memory is where the chunks come from; cy::core-values is
# where generational handles and Var come from; cy::core-reflect is what makes a struct a component;
# cy::core-jobs is <cy/core/jobs/access.h>, whose conflict checker gets its first real consumers
# here (task 2.5). cy::core-diagnostics joins as a PRIVATE dependency at task 2.12, when the ECS
# counters go on the M0 trace — private, so that including a component header does not drag the
# trace in behind it.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_ecs
    LAYER ecs
    ALIAS cy::ecs
    TYPE STATIC
    SOURCES
        src/archetype.cpp
        src/command_buffer.cpp
        src/component.cpp
        src/diagnostics.cpp
        src/entity.cpp
        src/query.cpp
        src/resource.cpp
        src/snapshot.cpp
        src/sparse_store.cpp
        src/state_schema.cpp
        src/system.cpp
        src/world.cpp
        src/world_relations.cpp
        src/world_stream.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values cy::core-reflect
                        cy::core-jobs
                        # cy::core-determinism for the state schema the built-in components declare
                        # themselves to (state_schema.h): a `Parent` divergence has to change the
                        # state hash, and reflection cannot describe an `Entity` field.
                        cy::core-determinism
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

# A suite lands with the first real source. The EXISTS guard is what lets this line be written now:
# it is the one line an agent adding tests/ does not then have to remember to add here.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

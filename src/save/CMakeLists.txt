# src/save/ — layer 2: durable game state. Section 6 of the M6 tasks.
#
# Governed by `save-and-persistence`. Reaches Working at M6.
#
# WHAT BELONGS HERE. The persistence overlay and the persistent state store organised by region;
# persistence scopes and the traits that route into them; persistent identity; dirty tracking; the
# save container — manifest, content-addressed chunks, structured load failures; migration on the
# way in; the journal and its compaction; atomic generations and their retention; the storage
# backend interface and its filesystem and in-memory implementations; and the service that captures
# on the main thread and writes in the background.
#
# WHAT DOES NOT BELONG HERE, AND WHY THE MODULE IS SMALL BECAUSE OF IT.
#
#   * A second serialisation path. `save-and-persistence` says the save IS the world's persistence
#     overlay and that "a second persistence model SHALL NOT exist", and M6's design.md §4 repeats
#     it. Nothing here re-implements a traversal, a wire format or a migration chain:
#     src/core/serialize/ owns all three, and this module composes them. If a reviewer finds a
#     second encoder in this directory, that is the defect.
#   * Any knowledge of the ECS, of a node, or of a cell's cooked bytes. An overlay is value records
#     addressed by persistent identity, and applying one to a live world is the caller's — which is
#     what lets a dedicated server, a replay and the editor's play mode share the model.
#   * The partitioner. `RegionKey` carries a cell identifier and interprets nothing about it,
#     because `world-partition-and-streaming` requires the encoding to be opaque to consumers.
#
# WHY LAYER 2 AND NOT LAYER 4. The module depends on nothing above layer 0 — core base, memory,
# reflect, values, serialize, assets and jobs — and it is consumed from above by world partition,
# by gameplay and by the editor's play mode. Declaring it at the layer its dependencies allow rather
# than at the layer of its first consumer is what keeps it available to all of them: a module
# declared at `scene` could not be used by anything at `servers`, and the persistent world is a
# service in that shape. It is not IN src/servers/ because it is not a singleton facade over a
# subsystem's live state; it is a data model plus a writer.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_save
    LAYER servers
    ALIAS cy::save
    TYPE STATIC
    SOURCES
        src/archive.cpp
        src/container.cpp
        src/identity.cpp
        src/overlay.cpp
        src/service.cpp
        src/storage.cpp
        src/traits.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-assets cy::core-jobs cy::core-serialize cy::core-values)

# A suite lands with the first real source. The EXISTS guard is what lets this line be written now:
# it is the one line an agent adding tests/ does not then have to remember to add here.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

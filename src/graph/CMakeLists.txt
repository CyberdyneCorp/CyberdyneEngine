# src/graph/ — CyberGraph: the shared authoring layer, the shared pure-expression SSA core, and the
# lowerings each consumer's own specification names. M8.b section 2.
#
# ================================================================================================
# WHY THIS IS THREE LAYERS AND NOT ONE INTERMEDIATE REPRESENTATION
# ================================================================================================
#
# The milestone's spike asked whether one graph IR could serve seven consumers and measured that it
# cannot: `design.md` §1.3 records five escape hatches against a budget of two, and
# `visual-scripting`'s own "No universal representation" requirement had already forbidden it by
# name. What is shared was measured too, and it is two things:
#
#   1. THE AUTHORING LAYER. `visual-scripting`: "The infrastructure SHALL own: node and pin models,
#      typed connections, stable identity, serialization, subgraphs, the editor canvas and its undo,
#      diffing and merging, versioning and migration, and debugging." That is `cybergraph.h` and the
#      four headers beside it, and every consumer adopts it.
#   2. THE PURE-EXPRESSION SSA CORE — hash-consing, canonical commutative ordering, canonical visit
#      order, constant folding, common-subexpression elimination, dead-node elimination, the
#      content-hashed digest and the provenance side table. Every domain has expression islands.
#      That is `expr.h`, `passes.h` and `emit.h`.
#
# What is NOT shared is the intermediate representation each consumer compiles to. `lower/` holds
# one per consumer, each to the form its own specification names, and none of them interprets a
# graph at runtime.
#
# `locomotion.cpp` is the one file here that AUTHORS a graph rather than compiling one: the
# four-state idle/walk/run/die machine, written as the nodes `lower_pose.cpp` understands and handed
# to `compile_pose()`. It exists because `animation-and-skinning` allows no graph compiler in the
# runtime and the editor cannot yet save an animation graph, so an engine with four imported clips
# and no authored asset would otherwise have no way to reach a `PoseProgram` at all. It is not a
# second compiler — see src/graph/README.md.
#
# `src/rendering/material/` IS NOT TOUCHED BY THIS MODULE and must not be. It is M7's closed work,
# the open operation table would invalidate its cook keys, and re-testing it mid-milestone buys
# nothing. Its criterion here is an ANCHOR instead: `tests/anchor/` expresses the material op table
# and type lattice as a domain over this core and reproduces the tree's own reference material at
# three measured digests. See tests/anchor/material_anchor.h.
#
# LAYER. `scene` (4): above cy::core (0) and below cy::runtime (5). There is no `graph` rung in
# `engine-architecture`'s table, and inventing one would be a change to cmake/module.cmake —
# src/gameplay/CMakeLists.txt and src/world/CMakeLists.txt both declined to make that change for the
# same reason and chose the same rung. Nothing here depends on anything above core: a graph compiler
# that needed a scene, a device or a server would be a compiler that cannot run in a cook.
cy_add_module(
    NAME cy_graph
    LAYER scene
    ALIAS cy::graph
    TYPE STATIC
    SOURCES
        src/audit.cpp
        src/cybergraph.cpp
        src/emit.cpp
        src/expr.cpp
        src/locomotion.cpp
        src/lower_behaviour.cpp
        src/lower_camera.cpp
        src/lower_pose.cpp
        src/lower_script.cpp
        src/lower_script_native.cpp
        src/merge.cpp
        src/passes.cpp
        src/text.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::core-values)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

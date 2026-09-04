# src/scene/serialization/ — layer 4: prefabs, scenes and worlds as authoring assets, and the cook
# that turns them into archetype blocks.
#
# Section 3.2 of tasks.md. Governed by `serialization-and-prefabs`.
#
# WHAT BELONGS HERE. The three asset kinds and their distinct semantics; the authoring text form and
# the cooked binary form of a scene; entity references by stable local id; prefabs, instance
# overrides addressing stable identifiers, variants, and exposed parameters; dependency-cycle
# rejection; the cook pipeline — resolve, bind parameters, validate, flatten, assign identity, emit
# entity templates — and live prefab update in development builds.
#
# WHAT DOES NOT BELONG HERE. The traversal itself: one reflection-driven traversal with two writers
# lives at layer 0 in src/core/serialize/ (design.md §6), so that a save, a replay and a cooked cell
# share it. This module knows what a prefab is; that one knows what a reflected field is.
#
# WHAT THE M2 SPIKE FIXED ABOUT THE COOK OUTPUT (tasks 0.1-0.4). A cooked block is chunk-shaped and
# copies in whole-chunk memcpys, but a copy alone does not activate it: the key column and every
# entity-reference slot hold cook-time local indices and are rewritten at activation. The cooker
# must therefore emit, beside each archetype block, the reference sites in it — the column and the
# byte offset within the component of every entity-typed field — so that the fixup is a strided pass
# over known columns rather than a reflection walk. Emitting that table is not optional: it is the
# difference between a fixup that costs a quarter of activation and one that costs many times it.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
#
# WHY THE DEPENDENCY LIST IS NOT cy::scene. The scaffold declared this module against cy::scene, and
# it does not use it: cooking reads authoring documents and writes archetype blocks, and a `Node` —
# which is a handle onto an entity, not a thing a file contains — appears nowhere in that path. What
# it does use is cy::ecs, directly and unavoidably: `World::instantiate`, `ArchetypeBlock` and
# `Entity` are what a cooked template is spawned through. Declaring the dependency that exists rather
# than the one the directory name suggests is what keeps the link graph honest, and the module is
# still at layer scene, so nothing about the layer check changes.
#
# cy::core-math is here for `cy::Transform`, which hierarchy flattening composes and bakes.
cy_add_module(
    NAME cy_scene_serialization
    LAYER scene
    ALIAS cy::scene-serialization
    TYPE STATIC
    SOURCES
        src/cook.cpp
        src/document.cpp
        src/document_binary.cpp
        src/document_text.cpp
        src/library.cpp
        src/overrides.cpp
        src/resolve.cpp
        src/spawn.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-serialize cy::core-math cy::ecs)

# A suite lands with the first real source. The EXISTS guard is what lets this line be written now:
# it is the one line an agent adding tests/ does not then have to remember to add here.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

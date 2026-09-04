# src/scene/ — layer 4: the node façade over the ECS, and the authoring formats that produce it.
#
# Section 3 of tasks.md. Governed by `scene-graph-and-nodes`; serialization/ below is governed by
# `serialization-and-prefabs`.
#
# WHAT BELONGS HERE. Node as a view onto an entity; hierarchy and naming; the transform model; node
# types and their components; visibility and enablement; lifecycle callbacks; behaviours bridging
# nodes and systems; groups and tags; scenes and worlds as runtime objects.
#
# WHAT DOES NOT BELONG HERE. Duplicated component data (design.md §3). A Node holds an entity id and
# a world reference; every property it exposes reads and writes through the ECS. There is no shadow
# copy, no dirty-flag pair and no sync step, because the moment one exists the two representations
# can disagree and every bug after that is a debugging session about which one was right. The
# coherence invariants are tests in this milestone (task 3.1.10), not prose.
#
# Also not here: any server (layer 2), any backend, and any allocation the ECS should be making.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
#
# node.cpp and node_transform.cpp are one class in two files on purpose. The split is the
# authored/derived line the whole module is organised around — identity, naming and hierarchy on one
# side, transforms and the two inherited flags on the other — and it keeps the file a reviewer opens
# to check "does a Node cache anything" down to something they can read in one sitting.
cy_add_module(
    NAME cy_scene
    LAYER scene
    ALIAS cy::scene
    TYPE STATIC
    SOURCES
        src/behaviour.cpp
        src/coherence.cpp
        src/components.cpp
        src/group.cpp
        src/node.cpp
        src/node_template.cpp
        src/node_transform.cpp
        src/propagation.cpp
        src/scene.cpp
        src/state_schema.cpp
        src/tree.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-values cy::ecs)

# The authoring side: prefabs, scenes, worlds, and the cooker that flattens them. Read after the
# façade it serialises, so cy::scene is already declared when cy::scene-serialization links it.
add_subdirectory(serialization)

# A suite lands with the first real source. The EXISTS guard is what lets this line be written now:
# it is the one line an agent adding tests/ does not then have to remember to add here.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

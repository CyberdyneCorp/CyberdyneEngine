# src/animation/ — CyberAnimation: skeletons and bone level of detail, clips and their codec, the
# compiled animation program's runtime, root motion, the constraint framework, retargeting, the GPU
# pose world and pose sharing. M8.b section 5.
#
# ================================================================================================
# WHAT IS HERE, AND WHAT IS DELIBERATELY NOT
# ================================================================================================
#
# THE COMPILER IS NOT HERE. `animation-and-skinning` asks for "graph -> typed IR -> optimisation ->
# compact program", and that is M8.b task 2.4's `cy/graph/lower_pose.h` — the animation IR with
# poses as values, a lazily-evaluated state machine and pose dependency analysis. This module holds
# the RUNTIME that program drives and the assets it names, which is the split the specification
# itself draws: "Compilation SHALL occur at cook time; the runtime SHALL contain no graph compiler."
# Nothing in src/animation/ parses or lowers a graph.
#
# THE POSE WORLD IS HERE, AND IT IS WHAT src/servers/render/geometry/ HAS BEEN WAITING FOR SINCE M6.
# `SkinningDescriptor::validate()` refused `PoseSource::GpuPoseWorld` naming M8; `pose_world.h` is
# the world that refusal was holding a place for.
#
# LAYER. `scene` (4). Above cy::core (0) and cy::servers (2), below cy::runtime (5) — the same rung
# src/graph/, src/gameplay/, src/physics/ and src/world/ declare, and for the reason each of them
# gives: there is no `animation` rung in `engine-architecture`'s table, and inventing one is a
# change to cmake/module.cmake that no milestone has yet had a reason to make.
#
# WHAT IT DEPENDS ON, AND WHAT IT MUST NOT.
#
#   cy::core-base, -math, -memory, -values   the vocabulary
#   cy::core-reflect                         `TypeId` and `FieldId`, which is how a property track
#                                            binds to a field it survives a rename of
#   cy::graph                                the compiled pose program, its state machine and its
#                                            joint masks
#
# THERE IS NO cy::ecs AND NO cy::scene HERE, and that is deliberate rather than incidental. This
# module evaluates poses; which entity owns one, and which stage the evaluation runs in, is the
# bridge's business — the same line src/servers/physics/ draws against src/physics/. A module at
# this layer that named a `World` would decide the scheduling policy for every consumer of a pose.
#
# CY_ANIMATION. The option gates this directory. `-D CY_ANIMATION=OFF` removes the animation runtime
# and its suites and leaves everything else — including `cy::graph`'s pose lowering, which is a
# compiler and belongs to `visual-scripting` — building and green. "the animation runtime SHALL be
# excluded, and static meshes SHALL render unaffected", which is exactly what nothing here being
# linked by the renderer means today.
cy_add_module(
    NAME cy_animation
    LAYER scene
    ALIAS cy::animation
    TYPE STATIC
    SOURCES
        src/clip.cpp
        src/evaluate.cpp
        src/ik.cpp
        src/lod.cpp
        src/pose_world.cpp
        src/retarget.cpp
        src/skeleton.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-reflect
                        cy::core-values cy::graph)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

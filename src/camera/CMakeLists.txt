# src/camera/ — CyberCamera's world half: the six things M4's Seed left, at the layer that can
# reach the world. M8.b task 7.3.
#
# WHAT IS HERE, AND WHY IT IS NOT src/servers/camera/. `camera-system` reached Seed at M4 in
# src/servers/camera/ at layer 2, and that module's README names what it left:
#
#   "Absent, not stubbed: framing and composition constraints, camera volumes, the strategy camera,
#    the director camera, aim assistance and screen/world projection. All are `camera-system`'s and
#    all are M8's."
#
# This module is those six. It is at layer 4 rather than 2 because each of them needs something
# layer 2 cannot name: a target resolved from the world, the world's spatial index, terrain height,
# `cy::render::ViewDescription`, and CyberGraph. It EXTENDS `namespace cy::camera` and reuses that
# module's `TargetBinding`, `TargetSample`, `Lens`, `EvaluatedCamera` and `CollisionParams` rather
# than declaring a second set: two definitions of a camera's target would be the conflation
# `camera-system`'s first requirement spends a page forbidding.
#
# LAYER. `scene` (4) — the rung src/graph/, src/gameplay/ and src/world/ all chose, for the reason
# each of their CMakeLists gives: `engine-architecture`'s table has no rung for these and inventing
# one is not this milestone's change to make.
#
# THE DEPENDENCIES.
#   cy::core-base       public   Expected, Error, the fixed-width types.
#   cy::core-math       public   Vec3, Quat, Transform, Aabb, Frustum, Ray.
#   cy::core-memory     public   Array and Span appear in public headers.
#   cy::core-values     public   Name.
#   cy::graph           public   `authoring.h` takes a `cy::graph::Graph`.
#   cy::servers-camera  public   The camera vocabulary this module extends. Layer 2, below.
#   cy::servers-render  public   `ViewDescription` is what `projection.h` reads. Layer 2, below.
#   cy::world           public   `WorldPosition` — the cell-relative form the specification requires.
#
# NOTHING HERE IS BEHIND A CY_* OPTION, for the reason src/servers/camera/ gives: a subsystem that
# exists in some configurations has a suite that runs in some configurations.
cy_add_module(
    NAME cy_camera
    LAYER scene
    ALIAS cy::camera
    TYPE STATIC
    SOURCES
        src/assist.cpp
        src/authoring.cpp
        src/framing.cpp
        src/projection.cpp
        src/strategy.cpp
        src/volume.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-values cy::graph
                        cy::servers-camera cy::servers-render cy::world)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

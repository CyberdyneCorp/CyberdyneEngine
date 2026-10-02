# src/animation/system/ — the per-frame animation system: the `Animator` component and the system
# that advances, evaluates and publishes every animated entity in `ecs::Stage::Animation`. Issue
# #76 stage 1.
#
# A MODULE OF ITS OWN, BESIDE THE RUNTIME AND NOT IN IT. src/animation/CMakeLists.txt says why the
# runtime names no `World`: which entity owns a pose and which stage evaluates it is "the bridge's
# business — the same line src/servers/physics/ draws against src/physics/". This is that bridge,
# and it depends on the ECS, the scene (for root motion applied to `LocalTransform`) and the job
# system, none of which the runtime may.
#
# LAYER. `scene` (4), with cy::physics-bridge, for the reason src/physics/ gives.
#
# It exists only with CY_ANIMATION: its parent directory is not added otherwise, so
# `-D CY_ANIMATION=OFF` removes the system with the runtime it drives.
cy_add_module(
    NAME cy_animation_system
    LAYER scene
    ALIAS cy::animation-system
    TYPE STATIC
    SOURCES
        src/animation_system.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::animation cy::core-base cy::core-determinism cy::core-jobs
                        cy::core-math cy::core-memory cy::ecs
    PRIVATE_DEPENDENCIES cy::scene)

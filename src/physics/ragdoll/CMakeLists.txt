# The skeleton-to-physics join lives at the scene layer and depends only on the engine interface.
# CY_ANIMATION gates it; CY_PHYSICS only selects whether Jolt or the reference backend runs it.
cy_add_module(
    NAME cy_physics_ragdoll
    LAYER scene
    ALIAS cy::physics-ragdoll
    TYPE STATIC
    SOURCES src/profile.cpp src/ragdoll.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::animation cy::core-base cy::core-math cy::core-memory
                        cy::servers-physics)

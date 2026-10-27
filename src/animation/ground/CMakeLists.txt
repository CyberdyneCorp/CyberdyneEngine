# src/animation/ground/ — the ground foot placement stands on, answered from physics or from
# terrain. Issue #92.
#
# A MODULE OF ITS OWN, BESIDE THE RUNTIME. `cy/animation/foot_placement.h` asks for the ground
# through `GroundProbe` and names no physics server and no terrain, because the runtime is
# backend-independent ("animation itself remains backend-independent"). These are the two probes a
# host hands it: a raycast through `cy::servers-physics`, which sees every collider a character
# can stand on, and a vertical cast through `cy::terrain`'s query, which needs no physics world.
#
# LAYER. `scene` (4), with the runtime and with cy::terrain.
cy_add_module(
    NAME cy_animation_ground
    LAYER scene
    ALIAS cy::animation-ground
    TYPE STATIC
    SOURCES
        src/ground_probes.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::animation cy::core-base cy::core-math cy::servers-physics cy::terrain)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

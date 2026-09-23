cy_add_module(
    NAME cy_physics_buoyancy
    LAYER scene
    ALIAS cy::physics-buoyancy
    TYPE STATIC
    SOURCES src/driver.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::servers-physics
                        cy::water cy::world)

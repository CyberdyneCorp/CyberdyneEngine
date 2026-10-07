# src/movement/ — the fixed-point kinematic mover and the Fixed navigation world it moves units in.
# openspec/changes/add-deterministic-math, stage 6 (design §8 and §9.1).
#
# WHAT THIS MODULE IS FOR. Authoritative units in a `Lockstep` session: positions, velocities and
# headings in `Fixed`, collision as circles and capsules on the plane against static navigation and
# height data and between units, and the path queries that feed them, all in integer arithmetic so
# every architecture computes the same bits. Physics stays presentation under `Lockstep` (design §8).
#
# LAYER. `scene` (4), beside src/navigation/, whose baked mesh it converts at load. It links
# `cy::core-detmath`, which defines `CY_DETERMINISM_MATH=1` for every consumer — so linking this
# module is how a game says it has deterministic math, and nothing below it inherits that claim.
#
# THE FLOAT BOUNDARY IS THREE FILES. convert_mesh.cpp and height_field_cook.cpp read baked floats and
# convert them once with `detmath::from_f32_cooked` (design §7.1); they never run in a tick.
# presentation.cpp converts the other way, `detmath::to_f32_relative` into the scene (design §7.2),
# on the presentation side of the tick. Every other source here is integer-only.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_movement
    LAYER scene
    ALIAS cy::movement
    TYPE STATIC
    SOURCES
        src/convert_mesh.cpp
        src/height_field.cpp
        src/height_field_cook.cpp
        src/mover.cpp
        src/nav_mesh.cpp
        src/path.cpp
        src/presentation.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-detmath cy::core-determinism cy::core-jobs
                        cy::core-memory cy::ecs cy::navigation cy::scene)

# Contraction off, as every authoritative module sets it. The integer paths have nothing to
# contract; the two conversion files are float, and the profile declaration below refuses to
# configure without the flag.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_movement PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_movement PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_movement
    PROFILE Lockstep
    REASON "every per-tick quantity is a detmath Fixed and every decision an exact integer comparison with an index tie-break; the only floating point is the load-time conversion in convert_mesh.cpp and height_field_cook.cpp and the presentation sync in presentation.cpp")

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

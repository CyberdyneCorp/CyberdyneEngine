# src/backends/physics-jolt/ — layer 3: Jolt Physics behind `cy::physics::PhysicsServer`.
# Task 4.2.2.
#
# `physics` — "Jolt as the 3D backend": engine handles map to Jolt body ids, engine shapes to Jolt
# shapes with a cache so identical shapes are shared, collision layers and masks to Jolt's
# broad-phase layers and object layer filters, Jolt's job system to the engine's, and Jolt's temp
# allocator to an engine arena.
#
# THE RULE THIS DIRECTORY EXISTS TO KEEP: no JPH type appears above src/backends/. Three things
# hold it, and the third is missing:
#   1. the interface is `cy::physics::PhysicsServer` at layer 2, which cannot include a layer-3
#      header at all — tools/layercheck/layercheck.py's `includes` check fails the build over it;
#   2. `cy::dep::jolt` is linked PRIVATE below, so no consumer inherits Jolt's include directories
#      and `#include <Jolt/Jolt.h>` does not even resolve outside this target;
#   3. what does NOT yet exist is a `physicsapi` check beside `gpuapi` and `sdl`, naming Jolt's
#      include root. Points 1 and 2 make the mistake a build failure everywhere it matters today,
#      but the belt-and-braces third check is what `gpuapi` is to Vulkan and it should be added.
#      See this milestone's report: layercheck.py is not this agent's file.
#
# GATED BY CY_PHYSICS, which is ON by default (cmake/features.cmake says why at length). The option
# gates this target AND the Jolt fetch in deps/manifest.toml, so a build with it off downloads
# nothing and still simulates through the reference backend.
#
# Sources are listed explicitly and never globbed.
cy_add_module(
    NAME cy_physics_jolt
    LAYER backends
    ALIAS cy::physics-jolt
    TYPE STATIC
    SOURCES
        src/jolt_jobs.cpp
        src/jolt_server.cpp
        src/jolt_shapes.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::servers-physics
    # PRIVATE, and that is the whole point: `cy::dep::jolt` is an implementation detail of this
    # target and a consumer inherits none of Jolt's include directories. A public link here would
    # put Jolt's headers on the command line of every translation unit that used physics, and the
    # layer checker would then be the only thing standing between the engine and a JPH type.
    PRIVATE_DEPENDENCIES cy::dep::jolt cy::core-jobs cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

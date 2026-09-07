# src/rendering/gi/ — layer 4: CyberGI. Tasks 9.1, 9.2 and 9.5.
#
# The GI scene, the sparse distance field, the surface and radiance caches, the screen, software and
# hardware tracing tiers with their sample confidence, the resolve, reflections on the same
# infrastructure, the reflection probes, the offline path tracer that bakes and provides ground
# truth, and the GI budget.
#
# `rendering-global-illumination` at Working for M7.
#
# NAMED SUBSYSTEMS, NOT A SINGLE ILLUMINATION MODULE. The specification requires the decomposition
# and one file per subsystem is how it is kept: `IlluminationSystem` in system.h composes them and
# owns none of their logic, and the two caches reach each other only through the abstract
# `IndirectSource` and `Occluder` seams in caches.h — which is what makes "the radiance cache is
# replaced and the rest is unaffected" a property of the link graph rather than a hope.
#
# NO DEVICE, NO GRAPH, NO SHADER. Every tier here is arithmetic over the GI scene, the field and the
# caches; the hardware tier reaches a device through cy::rendering-raytracing, which does not name
# one either and says why in its own README. That is what lets every case in tests/ run headless,
# and it is why the fallback tier — the one M7's exit criterion is about — is the path that actually
# executes on this machine.
cy_add_module(
    NAME cy_rendering_gi
    LAYER rendering
    ALIAS cy::rendering-gi
    TYPE STATIC
    SOURCES
        src/bake.cpp
        src/budget.cpp
        src/distance_field.cpp
        src/radiance_cache.cpp
        src/reflections.cpp
        src/resolve.cpp
        src/scene.cpp
        src/surface_cache.cpp
        src/system.cpp
        src/tracing.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory
                        cy::rendering-denoise cy::rendering-raytracing
    PRIVATE_DEPENDENCIES cy::core-jobs)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

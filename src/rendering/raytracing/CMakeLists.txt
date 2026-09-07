# src/rendering/raytracing/ — layer 4: the renderer service that owns acceleration structures.
#
# `ray-tracing-infrastructure` at Working for M7, task 9.4. Bottom-level lifecycle and refit policy,
# the top-level structure, a geometry adapter per geometry source, the ray query interface, the
# build budget, and the capability gate whose whole purpose is that turning ray tracing off on
# capable hardware behaves exactly like hardware that never had it.
#
# NO DEVICE AND NO VULKAN HEADER, AND THAT IS NOT A SIMPLIFICATION — IT IS THE LAYER RULE.
# tools/layercheck.py's `gpuapi` check allows a Vulkan header only under src/backends/, and
# `cy::rhi` has `Capability::RayTracing` as an enumerator that nothing sets: the Vulkan backend's
# capability block never mentions it, so every device this engine can open today reports no ray
# tracing. The service is therefore INACTIVE by default on this tree, which is exactly the state
# `ray-tracing-infrastructure` requires a device without the extension to produce, and the query
# executes over `cy::Bvh` on the CPU when a caller declares support. include/.../query.h says all of
# this at the top of the file, because a reader who does not know it will mistake the seam for the
# implementation.
#
# cy::core-jobs is here for one function — `jobs::monotonic_now_ns()`, which stamps the top-level
# rebuild time the diagnostics requirement asks for. It is the engine's monotonic clock and reading
# std::chrono directly beside it would be a second one.
cy_add_module(
    NAME cy_rendering_raytracing
    LAYER rendering
    ALIAS cy::rendering-raytracing
    TYPE STATIC
    SOURCES
        src/acceleration.cpp
        src/geometry.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory
    PRIVATE_DEPENDENCIES cy::core-jobs)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

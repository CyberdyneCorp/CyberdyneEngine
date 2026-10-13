# src/rendering/raytracing_gpu/ — layer 4: the acceleration service's structures on a device.
# Issue #35, stage 3.
#
# `ray-tracing-infrastructure` — "Ray tracing infrastructure is a renderer service". The service in
# src/rendering/raytracing/ keeps every decision (the build budget, refit against rebuild, eviction,
# the instances a top level traces) and names no device; this module is a `rt::StructureMirror` the
# service tells what it decided, and it records the same builds through the RHI's acceleration
# structures (cy/backends/rhi/acceleration.h). It names no graphics API either: the Vulkan calls are
# the backend's, below layer 4, which is the `gpuapi` rule tools/layercheck/layercheck.py enforces.
#
# A device without `Capability::RayTracing` cannot create one (`DeviceStructures::create` refuses
# with ErrorCode::Unsupported, which is the RHI's own refusal), and a consumer takes its software
# tier from that refusal.
cy_add_module(
    NAME cy_rendering_raytracing_gpu
    LAYER rendering
    ALIAS cy::rendering-raytracing-gpu
    TYPE STATIC
    SOURCES src/device_structures.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-raytracing
                        cy::rendering-graph cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

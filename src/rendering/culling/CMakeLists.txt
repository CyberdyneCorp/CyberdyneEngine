# src/rendering/culling/ — layer 4: a scene of instances becomes a per-view visible set. Task 4.4.3.
#
# `rendering-culling-and-lod` at Seed: spatial indexing over `cy::DynamicBvh`, frustum culling in the
# order the requirement fixes, discrete LOD selection from projected screen coverage, visibility
# ranges and HLOD, and shadow caster culling. Occlusion culling and GPU-driven culling are named by
# the same specification and are M6's — cull.h's header comment says which seams are open for them.
#
# NO DEVICE, NO GRAPH, NO SHADER. Culling is arithmetic over bounds, flags and layer masks, and it
# runs on the frame thread and its workers. That is what lets every case in tests/ run headless, and
# it is why cy::rhi and cy::rendering-graph are absent from the list below rather than merely unused.
#
# cy::core-jobs is a PUBLIC dependency because `CullOptions` names a `jobs::JobSystem*`: a caller has
# to be able to spell the type to pass one, and a forward declaration in the header would still leave
# the caller needing the real one.
#
# cy::servers-render-culling IS PUBLIC FOR THE SAME REASON AND ARRIVED AT M7 (task 5.2). gpu_bridge.h
# names `render::culling::GpuCullView`, `GpuDrawPayload` and `GpuCullCounters` in its signatures: it
# is the seam between the CPU visible sets here and the compute dispatch's buffers, and a caller
# cannot use it without being able to spell those types.
#
# THAT DEPENDENCY IS WHAT M6'S CLOSING GATE ASKED FOR. It recorded that `cy::servers-render-culling`
# was linked by nothing but its own test binaries; cy::rendering-forward links this module, so the
# renderer now links it too — and gpu_bridge.cpp is what makes that a consumption rather than an
# edge. Adding it changes nothing about this module's headless property: gpu_cull.h names no device,
# no graph and no shader either, which is the whole reason it lives at layer 2.
cy_add_module(
    NAME cy_rendering_culling
    LAYER rendering
    ALIAS cy::rendering-culling
    TYPE STATIC
    SOURCES
        src/cull.cpp
        src/gpu_bridge.cpp
        src/lod.cpp
        src/spatial.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-jobs cy::core-math cy::core-memory
                        cy::servers-render cy::servers-render-culling)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

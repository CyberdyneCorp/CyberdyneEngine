# src/servers/render/ — layer 2: the handle-based render server. Section 4.1 of the M3 tasks.
#
# `rendering-architecture` — the `RenderServer` owns all renderable state behind generational
# handles and has no knowledge of entities, nodes or scripts. That last clause is what puts it at
# layer 2, and layer 2 is BELOW src/backends/ (3) and src/rendering/ (4).
#
# WHAT THAT MEANS FOR THE DEPENDENCY LIST BELOW, AND WHY IT IS SHORT. There is no cy::rhi here and
# there cannot be: this module may not name a device, a command buffer, an image format the RHI
# declared, or a render graph. Everything it holds is expressed in engine types — cy::core-math's
# `Transform` and `Aabb`, cy::core-values' `Handle` and `Name`, cy::core-memory's `HandlePool`. The
# modules above (src/rendering/) own the device and consume this one.
#
# WHAT BELONGS HERE. The object families and their handles; the scene, view and instance model; the
# GPU scene as a publication interface; the simulation-to-render snapshot; mesh representation and
# vertex compression; the deterministic sort key; debug draw; frame and view statistics.
#
# WHAT DOES NOT. A pass, a pipeline, a barrier, an upload, an ECS query, a node. Extraction from the
# ECS is src/rendering/scene/'s, because it needs an ECS world and this module is forbidden one —
# which is the requirement being enforced by the build rather than remembered.
cy_add_module(
    NAME cy_servers_render
    LAYER servers
    ALIAS cy::servers-render
    TYPE STATIC
    SOURCES
        src/debug_draw.cpp
        src/gpu_scene.cpp
        src/mesh.cpp
        src/model.cpp
        src/server.cpp
        src/snapshot.cpp
        src/sort.cpp
        src/statistics.cpp
        src/types.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

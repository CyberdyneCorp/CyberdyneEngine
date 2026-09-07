# src/servers/render/geometry/ — layer 2: the GPU-side content the render server holds and the
# renderer above it consumes. M6 task 8.4.
#
# `rendering-geometry-and-resources` stays at **Working** at M6 and completes at M8, because its
# Skinning requirement reads the GPU pose world and `animation-and-skinning` is M8 — M6 task 10.8,
# decided at M6's gate. Mesh representation, vertex
# compression, LOD chains and instancing landed at M3 in ../mesh.h and ../model.h; what this module
# adds is the remainder of the capability — skinning and blend shapes, resource residency and
# lifetime with a per-category memory ledger, mip streaming, ring-buffered dynamic geometry, and the
# diagnostics that make all of it visible.
#
# ONE REQUIREMENT IS NOT MET, AND IT IS REPORTED RATHER THAN QUIETLY SATISFIED. Skinning must read
# bone matrices from the GPU pose world, which belongs to `animation-and-skinning` at M8. There is no
# pose world at M6 and this module must not invent a second one — inventing one is exactly the
# failure that requirement exists to prevent. `skinning.h` declares the seam, `PoseSource` names it,
# and `SkinningDescriptor::validate()` REFUSES `PoseSource::GpuPoseWorld` naming the milestone. See
# the header's own comment, and M6 task 10.8, which asks the milestone gate to decide whether the
# requirement is scoped or the tier is moved.
#
# NO DEVICE, NO BUFFER, NO UPLOAD. Layer 2 may not name one. Everything here is a description a
# module above turns into GPU work: a `SkinningDescriptor` says what a compute pass must do and
# `ResourceLedger` says what memory costs, and neither holds a handle. That is what lets tests/ run
# in every profile on a machine with no GPU, and it is the same split ../gpu_scene.h makes.
cy_add_module(
    NAME cy_servers_render_geometry
    LAYER servers
    ALIAS cy::servers-render-geometry
    TYPE STATIC
    SOURCES
        src/resources.cpp
        src/skinning.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

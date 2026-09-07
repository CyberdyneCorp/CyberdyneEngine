find_package(Threads REQUIRED)

# src/rendering/virtual_geometry/ — layer 4: CyberGeometry. M7 tasks 7.1 to 7.5.
#
# `virtual-geometry` — a mesh is a hierarchy of small triangle clusters streamed on demand and
# selected per cluster, so that rendering cost tracks the pixels on screen rather than the triangles
# in the asset. What lives here is the whole of it: the asset representation and its page format,
# the cluster hierarchy and its error metric, the simplifier the crack-free invariant rests on, the
# GPU geometry cache and its streaming feedback, GPU traversal and cluster culling, and the
# visibility buffer with its material resolve.
#
# WHY LAYER 4 AND NOT LAYER 2, which is where `virtual-texturing` sits. The two capabilities are
# shaped alike and this one crosses the line the other does not: cluster culling and traversal are
# COMPUTE DISPATCHES, the visibility buffer is a render target, and material resolve is a pass. A
# server is layer 2 and may not name a device, a buffer or a pipeline, so a virtual geometry system
# at layer 2 would have had to stop exactly where the requirement starts — "hierarchy traversal and
# cluster selection SHALL run entirely on the GPU". CyberTexture's own CMakeLists.txt says the same
# thing from the other side: its uploader and its sampling shaders "are src/rendering/ work and
# arrive with the visibility buffer at M7".
#
# THE MODULE IS NOT GATED BY CY_VIRTUAL_GEOMETRY, AND THE OPTION IS ON.
#
# Two separate statements, and both are deliberate.
#
# The option is ON because the capability is DELIVERED at this milestone and `delivery-roadmap`
# fails a capability at Working whose CY_* option defaults off. M3 shipped a Vulkan backend nothing
# built and M4 repeated it; the cost was that a regression was caught by whoever turned the option
# on rather than by continuous integration.
#
# The module compiles unconditionally because of what the option is *for*: "The system SHALL be
# removable at build time via `CY_VIRTUAL_GEOMETRY`, and every asset SHALL remain renderable through
# the traditional path when it is absent." What must disappear is the RENDERER'S USE of virtual
# geometry — the passes, the dispatches, the cache — not the cooker and not the asset reader, which
# the fallback path and the editor need either way. So the option gates the runtime path inside
# gpu.cpp and the frame that drives it, and the suites that check the hierarchy, the crack-free
# invariant and the cook run in every configuration. That is the same reading `CY_PHYSICS` was
# given: the Jolt backend is behind the option, the PhysicsServer is not.
#
# ITS ONLY SERVER DEPENDENCY IS cy::servers-residency, and it is PUBLIC because `GeometryCache`
# names `residency::PageKey` and `residency::ResidencyServer` in its interface: a caller has to be
# able to spell the type to register a budget. That is the same sideways layer-2-from-above
# dependency CyberTexture has, and the layer checker permits it.
cy_add_module(
    NAME cy_rendering_virtual_geometry
    LAYER rendering
    ALIAS cy::rendering-virtual-geometry
    TYPE STATIC
    SOURCES
        src/asset.cpp
        src/build.cpp
        src/cluster.cpp
        src/gpu.cpp
        src/residency.cpp
        src/simplify.cpp
        src/traversal.cpp
        src/visbuffer.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::core-assets
                        cy::servers-residency cy::rhi cy::rendering-graph
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

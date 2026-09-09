# src/rendering/assembly/ — layer 4: THE FRAME, out of the renderer's own parts. M8.b task 11.2.
#
# M7's closing gate: "`cy_rendering_forward`, `cy_rendering_material`, `cy_rendering_post`,
# `cy_rendering_shadows`, `cy_rendering_sky`, `cy_rendering_temporal`, `cy_rendering_gpu_culling`
# and `cy_rendering_virtual_texturing` are linked by NOTHING BUT THEIR OWN TEST BINARIES. Nothing in
# the tree assembles a frame out of the renderer's own parts."
#
# THE DEPENDENCY LIST IS THE FIX, AND IT IS ALSO THE TEST OF IT. All eight are here, and each one is
# CALLED rather than merely linked — a link with no call would satisfy the sentence above and none
# of what it meant. `tests/test_assembly.cpp` asserts on a number that comes out of each of them.
#
#   cy::rendering-forward             the pass order, the cluster grid, the draw list and its sort
#   cy::rendering-material            the GPU material table a draw's material indexes
#   cy::rendering-post                the chain, which is what decides the frame's feature set
#   cy::rendering-shadows             the page cache the frame spends its shadow budget through
#   cy::rendering-sky                 the sky view table and the irradiance the ambient term reads
#   cy::rendering-temporal            one jitter per frame, applied in one place
#   cy::rendering-gpu-culling         the cull dispatch, when a device that supports it is attached
#   cy::rendering-virtual-texturing   the feedback recording, resolve and read-back
#
# and beside them the four that were already linked by something: cy::rendering-culling (the spatial
# index and the CPU cull), cy::rendering-lighting (the GPU light record), cy::rendering-graph (the
# graph and its executor) and cy::rhi (the device the last two of the eight need).
#
# WHY LAYER 4 AND NOT LAYER 5. This module names a device, so it cannot be a server; it names no
# host, no window, no world and no runtime, so it is not the runtime either. It sits exactly where
# `forward/` sits and consumes it, which is what src/rendering/README.md's "everything that consumes
# the RHI and is not a backend" describes.
#
# WHAT IS NOT HERE. src/rendering/material/ is LINKED and USED and NOT MODIFIED — it is M7's closed
# work and one extension invalidates its cook keys. gi/, denoise/, raytracing/ and virtual_geometry/
# are absent because they were never the gate's complaint: they link each other, and adding them
# here before a consumer needs them would be the same mistake in the other direction.
#
# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_rendering_assembly
    LAYER rendering
    ALIAS cy::rendering-assembly
    TYPE STATIC
    SOURCES
        src/frame_assembly.cpp
        src/scene_index.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES
        cy::core-base
        cy::core-math
        cy::core-memory
        cy::rendering-culling
        cy::rendering-forward
        cy::rendering-gpu-culling
        cy::rendering-graph
        cy::rendering-lighting
        cy::rendering-material
        cy::rendering-post
        cy::rendering-shadows
        cy::rendering-sky
        cy::rendering-temporal
        cy::rendering-virtual-texturing
        cy::rhi
        cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

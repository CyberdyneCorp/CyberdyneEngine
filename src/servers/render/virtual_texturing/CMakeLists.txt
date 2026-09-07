# src/servers/render/virtual_texturing/ — layer 2: CyberTexture. M6 tasks 5.1 to 5.4.
#
# `virtual-texturing` — extremely large logical textures backed by only the pages a frame needs:
# virtual address spaces, page tables, tiles and borders, the shared physical caches, the resident
# mip tail, GPU feedback, predictive prefetch and runtime producers.
#
# WHY IT IS A MODULE OF ITS OWN RATHER THAN PART OF cy::servers-render. Two reasons, and the second
# is the one that matters. The first is size: this is nine hundred lines with a worker pool in it,
# and folding it into the render server would make that module's dependency list a poorer
# description of what it is. The second is that this module depends on cy::servers-residency, and
# cy::servers-render must not: the render server is where every module above reaches for the scene
# model, and giving it a transitive dependency on the residency policy would put the policy in every
# translation unit that draws anything. A sideways dependency between two layer-2 modules is what
# CyberCamera already does with cy::servers-render, and the layer checker permits it.
#
# WHAT LAYER 2 COSTS THIS MODULE, STATED PLAINLY BECAUSE IT SHAPES THE WHOLE INTERFACE. src/servers/
# sits BELOW src/backends/ (3) and src/rendering/ (4), so nothing here may name a device, an image,
# a buffer, a sampler, a shader or a render graph. What lives here is therefore the model and the
# policy-facing half: address encodings, page tables and their staged update lists, the tile cache's
# occupancy and its CPU staging bytes, the feedback ring, the producer interface, the residency
# requests. The Vulkan-side upload, the page table image and the sampling shaders are src/rendering/
# work and arrive with the visibility buffer at M7 — this module's `apply_staged()` list is exactly
# the buffer that uploader copies from, which is why the batching lives here rather than there.
#
# NOT GATED BY AN OPTION. The whole module compiles in every build and every suite runs in every
# configuration, for the reason CyberPhysics and CyberText are not gated either.
#
# THE MEMBER ORDER IN system.h IS LOAD-BEARING. `ProductionPool` is declared last so that it is
# destroyed first, joining every worker before a cache's staging bytes are freed. That is the same
# class of defect M5.5's gate found in Jolt's job bridge; tests/test_teardown.cpp tears this system
# down mid-production, in a loop, for that reason.
find_package(Threads REQUIRED)

cy_add_module(
    NAME cy_servers_render_virtual_texturing
    LAYER servers
    ALIAS cy::servers-render-virtual-texturing
    TYPE STATIC
    SOURCES
        src/address.cpp
        src/feedback.cpp
        src/page_table.cpp
        src/physical_cache.cpp
        src/producer.cpp
        src/system.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::servers-residency
    PRIVATE_DEPENDENCIES cy::core-diagnostics Threads::Threads)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

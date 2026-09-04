# src/backends/rhi/ — layer 3: the Rendering Hardware Interface, its null backend, and its Vulkan
# backend.
#
# Section 2 of the M3 tasks. Governed by `rhi-and-render-graph`, which reaches Working this
# milestone. The render graph that computes barriers over this interface is src/rendering/graph/ at
# layer 4 — above this, because it consumes the RHI and must never be consumable by it.
#
# WHAT BELONGS HERE. The explicit, Vulkan-shaped interface: buffers, textures with views and
# subresource ranges, samplers, shader modules, pipeline layouts, descriptor sets, graphics and
# compute pipelines, command buffers, queues, fences, semaphores, timestamp queries and debug
# labels. The hard limits, asserted at creation. The capability model. The backend registry and its
# fallback chain. And the one table that maps a declared access intent onto a pipeline stage, an
# access mask and an image layout — which is the whole of what the engine knows about
# synchronisation.
#
# WHAT DOES NOT BELONG HERE. A render graph (layer 4). A window (platform/). And, in the interface
# itself, a Vulkan type: cy_rhi below has no dependency on any graphics API, and that is checked
# rather than intended — cy_rhi_vulkan is a separate target and the layer checker fails the build if
# a Vulkan header is included outside it.
#
# THE ORDER OF THE THREE TARGETS IS THE ORDER THEY WERE WRITTEN IN, and design.md §1 says why: the
# null backend comes before Vulkan so that the interface is an interface rather than a wrapper over
# whichever Vulkan calls were convenient. Written afterwards it becomes a set of empty functions
# shaped by decisions Vulkan already made, and stops being a reference for what the RHI requires.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_rhi
    LAYER backends
    ALIAS cy::rhi
    TYPE STATIC
    SOURCES
        src/access.cpp
        src/backend.cpp
        src/types.cpp
        src/validation.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    # cy::core-values is where the generational handles come from; cy::core-memory is where Array,
    # HandlePool and the memory domain a backend reports GPU bytes against come from.
    PUBLIC_DEPENDENCIES cy::core-base cy::core-values cy::core-memory)

# The null backend, first (design.md §1) and always built: it is the last link in the fallback chain
# and the reason a rendering test can run in continuous integration on a machine with no GPU.
add_subdirectory(null)

# Vulkan, behind the same interface. Gated by CY_RENDERER_VULKAN, which also gates the fetch of
# volk, Vulkan-Headers and VMA in deps/manifest.toml — a disabled backend downloads nothing.
if(CY_RENDERER_VULKAN)
    add_subdirectory(vulkan)
endif()

# A suite lands with the first real source. The EXISTS guard is what lets this line be written now:
# it is the one line an agent adding tests/ does not then have to remember to add here.
if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

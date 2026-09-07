# src/backends/viewport/ — layer 3: the viewport transport's pixel half, on the engine's side.
# M7 task 5b.1.
#
# --- WHY THIS IS UNDER src/backends/ AND NOT UNDER src/rendering/ ---------------------------------
#
# Because it includes a Vulkan header, and `tools/layercheck/layercheck.py`'s `gpuapi` check fails
# the build on a Vulkan header anywhere else. That is not an accident of this module: exporting an
# image's memory as a dma-buf needs `VK_EXT_image_drm_format_modifier` and
# `VK_KHR_external_memory_fd` **at image creation**, and `cy::rhi::Device` deliberately has no way
# to ask for either — `TextureDescription` has no "shareable" and there is no accessor for the
# underlying `VkImage`, because a renderer that could reach one could branch on a backend.
#
# So this is a backend: a module that owns a graphics API and publishes an engine-typed interface
# over it. `include/cy/backends/viewport/publisher.h` names no Vulkan type, which is what lets
# `samples/05b-editor-window/runtime/` — layer tools — drive it.
#
# --- WHY IT IS NOT PART OF cy::rhi-vulkan -------------------------------------------------------
#
# `cy::rhi-vulkan` implements `cy::rhi::Device`, an interface with a null backend beside it and a
# validation layer around it. This module implements nothing of the sort: it owns its own instance,
# its own device and one queue, and it exists to hand images to another process. Folding it in would
# put a Unix socket and an `SCM_RIGHTS` message inside the renderer's device.
#
# It does mean two Vulkan devices in one process, and there is one hazard in that which
# `src/publisher.cpp`'s header states and guards: volk's dispatch tables are process-wide, so this
# module uses a `VolkDeviceTable` of its own and never calls `volkLoadDevice`.
#
# --- LINUX --------------------------------------------------------------------------------------
#
# dma-buf, `memfd` and `SCM_RIGHTS` are Linux, and so is the editor's side of this transport
# (`editor/crates/cy-editor-viewport-transport/`, whose own manifest compiles to a constant on
# macOS and Windows). The module is not declared elsewhere, so a host that links it fails to
# configure rather than failing to import an image at run time.

if(NOT CY_RENDERER_VULKAN)
    message(STATUS "src/backends/viewport: CY_RENDERER_VULKAN is off, so there is no device to "
                   "publish a frame from; the module is not declared")
    return()
endif()

if(NOT LINUX)
    message(STATUS "src/backends/viewport: the viewport transport is dma-buf, memfd and SCM_RIGHTS, "
                   "which this platform does not have; the module is not declared")
    return()
endif()

cy_add_module(
    NAME cy_backends_viewport
    LAYER backends
    ALIAS cy::backends-viewport
    TYPE STATIC
    SOURCES
        src/publisher.cpp
        src/wire.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory
    # PRIVATE, so no consumer inherits a Vulkan include directory. That is the same argument
    # src/backends/rhi/vulkan/ makes, and it is what keeps the `gpuapi` check a structural guarantee
    # rather than a convention.
    PRIVATE_DEPENDENCIES cy::dep::vulkan_headers cy::dep::volk)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

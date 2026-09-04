# src/backends/rhi/vulkan/ — layer 3: Vulkan behind the RHI. Section 2.3 of the M3 tasks.
#
# Gated by CY_RENDERER_VULKAN, which also gates the fetch of volk, Vulkan-Headers and VMA in
# deps/manifest.toml: a build with the option off downloads nothing, compiles nothing here, and
# links no Vulkan code at all — which is `build-system-and-platforms`' rule that disabling a feature
# excludes it from the link rather than stubbing it at run time, and is what a dedicated server
# build depends on.
#
# THE THREE DEPENDENCIES, AND WHY EACH IS HERE.
#   vulkan_headers  the API's definition; headers only, nothing built, nothing linked.
#   volk            a meta-loader. Entry points are fetched per device rather than through the
#                   loader's dispatch trampoline, so the engine links no Vulkan library and still
#                   builds on a machine with no driver — which is what lets `just test-all` cover
#                   this backend's compile in continuous integration.
#   vma             suballocation, memory-type selection and VK_EXT_memory_budget tracking. GPU
#                   bytes reach the engine through cy::MemoryDomain::Gpu and the shared pressure
#                   monitor, not through a second report beside the CPU one.
#
# All three are PRIVATE. A Vulkan type in this module's public header would be a Vulkan type above
# src/backends/ the moment anything included it, and tools/layercheck/layercheck.py's `gpuapi` check
# fails the build on exactly that.
cy_add_module(
    NAME cy_rhi_vulkan
    LAYER backends
    ALIAS cy::rhi-vulkan
    TYPE STATIC
    SOURCES
        src/vulkan_command_buffer.cpp
        src/vulkan_device.cpp
        src/vulkan_instance.cpp
        src/vulkan_memory.cpp
        src/vulkan_swapchain.cpp
        src/vulkan_translate.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    # Set on the target rather than in one source, so every translation unit that sees VMA's header
    # sees the same configuration. VK_NO_PROTOTYPES means there are no symbols for VMA to link, and
    # dynamic loading is how it gets the entry points volk resolved.
    PRIVATE_DEFINITIONS VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    PUBLIC_DEPENDENCIES cy::rhi
    PRIVATE_DEPENDENCIES cy::dep::vulkan_headers cy::dep::volk cy::dep::vma)

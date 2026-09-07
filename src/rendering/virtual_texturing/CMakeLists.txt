# src/rendering/virtual_texturing/ — layer 4: CyberTexture's device half. M7 tasks 4.1, 4.2 and 4.3.
#
# `src/servers/render/virtual_texturing/README.md` names this module in advance and says what it
# owes: "The Vulkan-side upload, the page table image, the sampling shaders and the analytic
# derivative reconstruction under a visibility buffer are `src/rendering/` work and arrive with M7."
#
# WHY IT IS NOT PART OF cy::servers-render-virtual-texturing. That module is layer 2 and, in its own
# words, "nothing here may name a device, an image, a buffer, a sampler, a shader or a render graph".
# This module names all six. The split is that module's and this one only obeys it.
#
# WHY frame_residency.{h,cpp} IS HERE AND NOT IN src/servers/residency/. It is the CONSUMER, not the
# policy: it turns one frame's two GPU feedback streams into `residency::Request`s and asks the
# server to arbitrate between them. The policy stays where it was — `residency` owns scoring, budgets
# and eviction, and there is still no method in this module that chooses which page to drop.
#
# THE SPIR-V IS CHECKED IN, not compiled by the build. shaders/vt_frame.slang carries the three
# slangc invocations and shaders/embed_spirv.py turns their output into src/vt_frame_spirv.h. Same
# arrangement, same reason, as src/rendering/gpu_culling/ and tests/render/shaders/: CY_SHADER_SLANG
# is off in Profile and Shipping.
cy_add_module(
    NAME cy_rendering_virtual_texturing
    LAYER rendering
    ALIAS cy::rendering-virtual-texturing
    TYPE STATIC
    SOURCES
        src/frame.cpp
        src/frame_residency.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::rendering-graph
                        cy::servers-render-culling cy::servers-render-virtual-texturing
                        cy::servers-residency
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

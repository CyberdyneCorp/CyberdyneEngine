# src/rendering/occlusion/ — layer 4: ground-truth ambient occlusion on the device.
#
# `rendering-post-processing` — "Ambient occlusion". The horizon search, the shared denoiser's
# spatial cascade over its output, and the persistent target the forward pass samples. The frame
# reaches it through `FrameStageDeclaration` at `FramePassKind::AmbientOcclusion`, so this module
# names the forward frame's types and the forward frame names nothing of this one.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/skinning/CMakeLists.txt gives: CY_SHADER_SLANG is off in Profile and Shipping, and a
# renderer whose ambient occlusion exists only where Slang was built is a renderer with two frames.
# shaders/regenerate.py rewrites src/occlusion_spirv.h and src/occlusion_msl.h.
cy_add_module(
    NAME cy_rendering_occlusion
    LAYER rendering
    ALIAS cy::rendering-occlusion
    TYPE STATIC
    SOURCES src/gtao.cpp src/occlusion_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward cy::rendering-post
    PRIVATE_DEPENDENCIES cy::rhi cy::rendering-denoise)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

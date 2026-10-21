# src/rendering/upscale/ — layer 4: temporal upscaling on the device.
#
# `rendering-post-processing` — "Temporal upscaling": the scene rendered at a reduced internal
# extent, reconstructed to the output extent from its jittered samples, the prepass motion vectors
# and a history kept at the output extent, with disocclusion rejection, neighbourhood clipping, a
# reactive mask and a sharpening pass. The frame reaches it through `FrameStageDeclaration` at
# `FramePassKind::Temporal`, so this module names the forward frame's types and the forward frame
# names nothing of this one — the arrangement motion_blur/ and occlusion/ have.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites src/upscale_spirv.h
# and src/upscale_msl.h.
cy_add_module(
    NAME cy_rendering_upscale
    LAYER rendering
    ALIAS cy::rendering-upscale
    TYPE STATIC
    SOURCES src/upscale.cpp src/upscale_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward cy::rendering-temporal cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

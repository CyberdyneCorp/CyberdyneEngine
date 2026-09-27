# src/rendering/grading/ — layer 4: exposure and colour grading on the device.
#
# `rendering-post-processing` — "Exposure" and "Colour grading", at steps 6, 10, 11 and 12 of its
# chain. Three compute dispatches meter the scene-referred colour into a 256-bin luminance histogram
# and adapt an EV100 toward its percentile mean; a graded resolve applies that exposure (or a manual
# one), the tone curve and one lookup into a baked grading table, straight into the frame's output.
# The frame reaches it through `FramePassKind::PostProcess`'s callback, so this module names the
# forward frame's types and the forward frame names nothing of this one. The arithmetic — the
# metering, the adaptation, the `.cube` parser, the display log encoding and the bake — is
# cy::rendering-post's, and every shader expression has its host twin there.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/skinning/CMakeLists.txt gives: CY_SHADER_SLANG is off in Profile and Shipping.
# shaders/regenerate.py rewrites src/grading_spirv.h and src/grading_msl.h.
cy_add_module(
    NAME cy_rendering_grading
    LAYER rendering
    ALIAS cy::rendering-grading
    TYPE STATIC
    SOURCES src/grading_renderer.cpp src/look_file.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward cy::rendering-post
    PRIVATE_DEPENDENCIES cy::rhi cy::core-assets)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

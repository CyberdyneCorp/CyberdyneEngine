# src/rendering/motion_blur/ — layer 4: motion blur on the device.
#
# `rendering-post-processing` — "Motion blur": a tile reduction of the prepass velocity, the
# neighbourhood's dominant direction, and a depth-aware gather along it, scaled by the shutter
# angle. The frame reaches it through `FrameStageDeclaration` at `FramePassKind::MotionBlur`, so this
# module names the forward frame's types and the forward frame names nothing of this one — the
# arrangement contact_shadows/ and occlusion/ have.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites
# src/motion_blur_spirv.h and src/motion_blur_msl.h.
cy_add_module(
    NAME cy_rendering_motion_blur
    LAYER rendering
    ALIAS cy::rendering-motion-blur
    TYPE STATIC
    SOURCES src/motion_blur.cpp src/motion_blur_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward cy::rendering-post
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

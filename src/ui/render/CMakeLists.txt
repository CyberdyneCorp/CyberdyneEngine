# src/ui/render/ — CyberUI's primitive stream drawn on the device.
#
# `flatten()` produces primitives and batches in reference units; this target encodes them into the
# rows one shader reads, turns each batch into an indexed indirect draw with its clip as a scissor,
# and declares the pass at the frame's `UiAndDebug` stage through `FrameStageDeclaration`, after the
# tone curve and the selection outlines. encode.h is the device-free half — the rows, the scissors,
# the draws, and the shader transcribed for the suites — and ui_renderer.h the device half.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites src/ui_spirv.h and
# src/ui_msl.h.
#
# LAYER. `rendering` (4): it names a device and the render graph, which `cy::ui` must not.
cy_add_module(
    NAME cy_ui_render
    LAYER rendering
    ALIAS cy::ui-render
    TYPE STATIC
    SOURCES
        src/encode.cpp
        src/ui_renderer.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::ui cy::rendering-graph cy::rendering-forward cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

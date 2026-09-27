# src/rendering/depth_of_field/ — layer 4: depth of field on the device.
#
# `rendering-post-processing` — "Depth of field", step 7 of its chain. Five compute dispatches: a
# half-resolution layer of colour and signed circle of confusion, the near field's reach in tiles
# and their dilation, a scatter-as-gather of the far and near fields, and a full-resolution
# composite before bloom. The frame reaches it through `FrameStageDeclaration` at
# `FramePassKind::DepthOfField`, so this module names the forward frame's types and the forward frame
# names nothing of this one. The lens — the thin-lens circle of confusion — is cy::rendering-post's
# `circle_of_confusion`, and every shader expression has its host twin in src/focus.cpp.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites src/dof_spirv.h and
# src/dof_msl.h.
cy_add_module(
    NAME cy_rendering_depth_of_field
    LAYER rendering
    ALIAS cy::rendering-depth-of-field
    TYPE STATIC
    SOURCES src/focus.cpp src/dof_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward cy::rendering-post cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

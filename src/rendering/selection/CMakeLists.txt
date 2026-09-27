# src/rendering/selection/ — layer 4: selection outlines and unit highlights.
#
# `editor-viewport-and-gizmos` puts "selection outlines, highlight rendering, and overlays" on the
# engine's side of the split, and `editor-visual-language` fixes what a selection looks like. This is
# the engine half both describe, and the half a strategy game's unit selection draws through: a mask
# of the objects a game marked, drawn with the frame's own geometry and bindings, and an edge pass
# over the tonemapped colour. The frame reaches it through `FrameStageDeclaration` at
# `FramePassKind::SelectionOutlines`, as it reaches contact_shadows/.
#
# It links cy::rendering-pipeline because the mask draws what `FrameRecorder` draws — its geometry,
# its draw records and its sets 0 and 1 — so the outline and the object cannot disagree about where
# the object is.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites src/selection_spirv.h
# and src/selection_msl.h.
cy_add_module(
    NAME cy_rendering_selection
    LAYER rendering
    ALIAS cy::rendering-selection
    TYPE STATIC
    SOURCES src/highlight.cpp src/outline_pass.cpp src/selection_component.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-memory cy::ecs cy::rendering-graph
                        cy::rendering-forward cy::rendering-pipeline cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

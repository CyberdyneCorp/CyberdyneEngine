# src/rendering/viewmodel/ — layer 4: the first-person weapon view. `camera-system` — "One camera,
# several views"; `rendering-forward-clustered` — "Pass order".
#
# The arms and the weapon drawn with their own field of view and near plane INTO the world's frame:
# a projection whose depth is squeezed into the slice of the range nearest the eye, the assembly's
# second cull and the mark its draws carry, the view-block words the frame's `cyViewmodel*` vertex
# entries read, and last frame's lens for the weapon's motion vectors. No pass and no shader of its
# own: the frame's pipelines draw the weapon (`PipelineSetup::viewmodel`), so this module names the
# assembly's and the pipeline layer's types and neither of them names anything of this one.
cy_add_module(
    NAME cy_rendering_viewmodel
    LAYER rendering
    ALIAS cy::rendering-viewmodel
    TYPE STATIC
    SOURCES src/viewmodel.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::rendering-assembly cy::rendering-pipeline
                        cy::rendering-temporal cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

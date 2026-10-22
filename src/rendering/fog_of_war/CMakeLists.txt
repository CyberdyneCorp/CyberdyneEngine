# src/rendering/fog_of_war/ — layer 4: fog of war, drawn. `rendering-post-processing` — "Fog of war".
#
# One team's visibility as a texture the frame samples — faded in time and smoothed at its edges on
# the host (`VisibilityField`), uploaded once a frame and applied to every surface by one compute
# dispatch at `FramePassKind::FogOfWar` (`FogOfWarPass`) — and the same field as the minimap's
# overlay image (`compose_minimap_fog`). The frame reaches it through `FrameStageDeclaration`, so
# this module names the forward frame's types and the forward frame names nothing of this one.
#
# IT READS THE SIMULATION AND NEVER WRITES IT. The grid is `cy::gameplay::fog::FogOfWar`'s, kept in
# integers in the fixed step; this module takes it as one state byte a cell and links no gameplay
# code, so nothing it does can make two lockstep peers disagree.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites
# src/fog_of_war_spirv.h and src/fog_of_war_msl.h.
cy_add_module(
    NAME cy_rendering_fog_of_war
    LAYER rendering
    ALIAS cy::rendering-fog-of-war
    TYPE STATIC
    SOURCES src/fog_of_war_pass.cpp src/minimap.cpp src/visibility_field.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

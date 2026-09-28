# src/rendering/fog/ — layer 4: volumetric fog and light shafts on the device.
#
# `rendering-post-processing` — "Volumetric fog": a froxel volume filled with a height fog and
# authored fog volumes, lit by the sun through the directional shadow map and by the frame's ambient
# term, integrated front to back into a texture the forward pass reads to attenuate and add to every
# surface. The frame reaches it through `FrameStageDeclaration` at `FramePassKind::VolumetricFog`,
# so this module names the forward frame's types and the forward frame names nothing of this one —
# the arrangement contact_shadows/ has. The froxel distribution, the Henyey-Greenstein phase and the
# analytic slice integral are `rendering-post-processing`'s own, from cy::rendering-post.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites src/fog_spirv.h and
# src/fog_msl.h.
cy_add_module(
    NAME cy_rendering_fog
    LAYER rendering
    ALIAS cy::rendering-fog
    TYPE STATIC
    SOURCES src/fog_pass.cpp src/medium.cpp src/volume.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward cy::rendering-post cy::rendering-sky
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

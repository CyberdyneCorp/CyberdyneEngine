# src/rendering/lights_gpu/ — layer 4: many lights and area lights on the device.
# `rendering-lighting-and-shadows` — "Light types", "Area light evaluation", "Light culling and
# limits", "Stochastic many-light direct lighting"; `rendering-forward-clustered` — "Cluster light
# assignment".
#
# The device half of cy::rendering-lighting, which stays device-free: what a light is, how an area
# light shades and what a reservoir is are its; this module uploads the LTC table, assigns the light
# lists on the device and runs the stochastic path, each transcribing the host's arithmetic so a
# device suite can hold one to the other. The frame reaches it through `FrameStageDeclaration`, so
# this module names the forward frame's types and the forward frame names nothing of this one.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites
# src/lights_gpu_spirv.h and src/lights_gpu_msl.h.
cy_add_module(
    NAME cy_rendering_lights_gpu
    LAYER rendering
    ALIAS cy::rendering-lights-gpu
    TYPE STATIC
    SOURCES src/compute.cpp src/light_assignment.cpp src/ltc_texture.cpp src/many_lights.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-denoise
                        cy::rendering-forward cy::rendering-graph cy::rendering-lighting
                        cy::rendering-pipeline cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# src/rendering/contact_shadows/ — layer 4: screen-space contact shadows on the device.
#
# `virtual-shadows` — "Contact and traced refinement": a short screen-space trace toward the
# directional light through the prepass depth, written into a persistent target the forward pass
# reads as an ADDITION to the shadow map's visibility. The frame reaches it through
# `FrameStageDeclaration` at `FramePassKind::ContactShadows`, so this module names the forward
# frame's types and the forward frame names nothing of this one — the arrangement occlusion/ has.
#
# THE SPIR-V AND MSL ARE CHECKED IN, not compiled by the build, for the reason
# src/rendering/occlusion/CMakeLists.txt gives. shaders/regenerate.py rewrites src/contact_spirv.h
# and src/contact_msl.h.
cy_add_module(
    NAME cy_rendering_contact_shadows
    LAYER rendering
    ALIAS cy::rendering-contact-shadows
    TYPE STATIC
    SOURCES src/contact.cpp src/contact_pass.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-graph
                        cy::rendering-forward
    PRIVATE_DEPENDENCIES cy::rhi)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

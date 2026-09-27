# src/rendering/light_probes/ — layer 4: an irradiance volume on the device.
#
# `rendering-global-illumination` — "Irradiance volumes and light probes". The volume — capture,
# update policy, sampling rule — is `gi::IrradianceVolume` in cy::rendering-gi, which runs every
# case headless and links no device. The frame is cy::rendering-pipeline, which knows nothing of GI.
# This module is the one place the two meet: the probes as a texture the forward pass samples, and
# the view-block words that place the volume in the frame. It is a third module for the reason
# src/rendering/sky_illumination/ is one: an edge between the other two would make GI depend on the
# frame or the frame depend on GI.
#
# NO SHADER OF ITS OWN. The sampling is `cy/frame.slang`'s `probeVolumeAmbient`, in the forward
# fragment's committed SPIR-V and MSL; this module only fills what that function reads.
cy_add_module(
    NAME cy_rendering_light_probes
    LAYER rendering
    ALIAS cy::rendering-light-probes
    TYPE STATIC
    SOURCES src/probe_volume_texture.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::rendering-gi
                        cy::rendering-pipeline cy::rhi
    PRIVATE_DEPENDENCIES cy::rendering-graph)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

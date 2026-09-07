# src/rendering/sky/ — layer 4: CyberSky's Seed. An analytic atmosphere, the celestial model, and
# the sky as a LIGHT. M7 task 10.4.
#
# `atmosphere-sky-and-clouds` at **Seed**. What is here is the half a GI system consumes and the
# half a shader cannot compute for itself:
#
#   atmosphere.h  a participating atmosphere in physical units — Rayleigh, Mie, ozone, ground albedo
#                 and stellar illuminance — with transmittance, single scattering, the sun's own
#                 disc, the sunlight that reaches the ground, and aerial perspective from the SAME
#                 parameters as the sky. There is no colour constant in the file: a planet's sky
#                 follows from its coefficients, which is what the requirement asks for.
#   celestial.h   sun and moon directions from time, latitude and axial tilt, on a DECLARED time
#                 domain; and a model a project bypasses by writing a direction, which is the other
#                 half of the same requirement.
#   sky_light.h   the sky as a light: spherical-harmonic irradiance, the three-colour gradient
#                 `rendering-global-illumination` wants, and the sky view table a frame samples
#                 instead of ray marching.
#
# NO DEVICE, NO SHADER AND NO DEPENDENCY ON cy::rendering-gi. The last of those is deliberate and it
# is what makes task 10.4's "sufficient for GI's sky term" a measurement: the gradient fit is
# checked against the full atmosphere's own irradiance integral, in this module's own suite, with no
# global illumination system present. `sky_light.h` carries the three-line adapter the composition
# point writes.
cy_add_module(
    NAME cy_rendering_sky
    LAYER rendering
    ALIAS cy::rendering-sky
    TYPE STATIC
    SOURCES
        src/atmosphere.cpp
        src/celestial.cpp
        src/sky_light.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

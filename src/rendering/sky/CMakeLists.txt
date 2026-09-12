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
# M10 TASK 3.3 ADDED FIVE FILES AND FOUR DEPENDENCIES. Each dependency is a requirement of
# `atmosphere-sky-and-clouds` made into a build fact, and each is named here rather than left for a
# reader to infer:
#
#   cy::environment        THE CLOUD SHADOW FIELD. "Clouds SHALL cast shadows onto the world through
#                          a coarse world-scale shadow representation ... consumed by terrain,
#                          foliage, water, and illumination." That is CyberField's job, and M10
#                          section 1 built it. `src/cloud_shadows.cpp` declares one field, claims it
#                          with a `ProducerToken`, and writes tiles; the four consumers read it
#                          through `FieldReader` like any other field.
#   cy::rendering-post     THE FROXEL VOLUME. "Clouds, fog, mist, smoke, and atmospheric volumes
#                          SHALL share the engine's volumetric infrastructure rather than each
#                          building its own pipeline: the froxel volume defined in
#                          `rendering-post-processing`." `AerialPerspectiveTable` IS a
#                          `rendering::FroxelVolume` and calls `froxel_slice_depth()` and
#                          `froxel_slice_of()` rather than re-deriving the distribution; the cloud
#                          march calls `henyey_greenstein()` from the same header.
#   cy::rendering-temporal THE HISTORY CLASSIFIER. "Rendering SHALL run at reduced resolution with
#                          temporal reconstruction THROUGH THE FRAMEWORK IN `temporal-rendering`."
#                          `reproject_cloud()` calls `classify_history()`; what it adds is the
#                          cloud's own motion and the weather rejection, which is the
#                          cloud-specific half the requirement names.
#   cy::rendering-arbiter  THE PRICED LADDER. "Tier selection SHALL follow the renderer profile and
#                          the budget arbiter." `sky_quality_ladder()` returns a
#                          `rendering::QualityLadder` in the arbiter's own currency.
#
# AND ONE DEPENDENCY THAT IS DELIBERATELY ABSENT: cy::rendering-shadows. "Cloud shadows SHALL NOT be
# produced through the virtual shadow page system." There is no expression in this module that can
# allocate a page, because the target that owns them is not linked — the mechanism is unreachable
# rather than merely unused. See src/cloud_shadows.cpp.
cy_add_module(
    NAME cy_rendering_sky
    LAYER rendering
    ALIAS cy::rendering-sky
    TYPE STATIC
    SOURCES
        src/atmosphere.cpp
        src/celestial.cpp
        src/cloud_shadows.cpp
        src/clouds.cpp
        src/composition.cpp
        src/diagnostics.cpp
        src/profile.cpp
        src/sky_light.cpp
        src/tables.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::environment cy::rendering-arbiter cy::rendering-post
                        cy::rendering-temporal cy::world)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

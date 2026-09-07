# src/rendering/lighting/ — layer 4: lights in physical units, and the conventional shadow path.
# Tasks 4.4.1 and 4.4.2.
#
# `rendering-lighting-and-shadows` at Seed. What is here is the CPU half — the half that decides
# numbers a shader then uses:
#
#   units.h         lux, lumens, candela and nits, the black-body tint, and camera exposure. The
#                   answer to "what does 'physical units' mean" is a set of conversions, one of which
#                   (the spot's) is a real choice and is written down where it is made.
#   lights.h        the 64-byte record cy/light.slang reads, asserted field by field, built
#                   camera-relative in f64 so a light a million units out reaches the GPU exact.
#   shadow_atlas.h  tile sizing with hysteresis, allocation with retention, and cross-frame caching.
#   cascades.h      split distances, bounding-sphere stabilisation and texel snapping.
#   filtering.h     the sample counts a specialization constant takes, and the three-term bias.
#
# M7 task 10.3 adds the five the milestone names, on the same terms — numbers a shader then uses,
# checkable without a device:
#
#   area.h            linearly transformed cosines for rect, disc, sphere and tube lights; the
#                     moment-matched LTC table and exactly what it is and is not; the closed-form
#                     spherical-polygon integral with its horizon clip; the representative-point
#                     fallback for shading models with no LTC, with the approximation written down
#                     as a string a diagnostic prints.
#   decals.h          the projected oriented box, its two fades, reoriented normal blending, and a
#                     decal budget the ARBITER sets whose eviction is deterministic and reported.
#   light_functions.h cookies for the three light types, projected in light space, scrolled without
#                     touching a shadow map.
#   channels.h        a bitfield test at assignment and nothing at shading time, plus the bounded
#                     deterministic assignment that goes with it.
#   many_light.h      reservoir sampling with temporal and spatial reuse, the ray budget that
#                     replaces the per-cluster bound, and the configuration rule that refuses the
#                     path without a denoiser.
#
# NO DEVICE AND NO SHADER COMPILER, which is the same division src/rendering/material/ draws and for
# the same reason: a shadow that swims is a matrix that moved, and a matrix is testable without a
# GPU. The passes that consume all of this are src/rendering/forward/'s.
cy_add_module(
    NAME cy_rendering_lighting
    LAYER rendering
    ALIAS cy::rendering-lighting
    TYPE STATIC
    SOURCES
        src/area.cpp
        src/cascades.cpp
        src/channels.cpp
        src/decals.cpp
        src/filtering.cpp
        src/light_functions.cpp
        src/lights.cpp
        src/many_light.cpp
        src/shadow_atlas.cpp
        src/units.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

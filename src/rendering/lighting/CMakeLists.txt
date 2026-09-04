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
# NO DEVICE AND NO SHADER COMPILER, which is the same division src/rendering/material/ draws and for
# the same reason: a shadow that swims is a matrix that moved, and a matrix is testable without a
# GPU. The passes that consume all of this are src/rendering/forward/'s.
cy_add_module(
    NAME cy_rendering_lighting
    LAYER rendering
    ALIAS cy::rendering-lighting
    TYPE STATIC
    SOURCES
        src/cascades.cpp
        src/filtering.cpp
        src/lights.cpp
        src/shadow_atlas.cpp
        src/units.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::core-memory cy::servers-render)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

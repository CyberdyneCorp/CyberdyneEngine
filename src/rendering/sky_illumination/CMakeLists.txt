# src/rendering/sky_illumination/ — layer 4: THE ATMOSPHERE BECOMES ILLUMINATION. M11.c tasks 2.1–2.4.
#
# THIS FILE IS THE CRITERION. `m11c:gi-sky-term-constructed` is about a LINK-GRAPH FACT and not
# about a paragraph: [dependency cycle 2](docs/roadmap/dependencies.md) says the physical atmosphere
# lands at M10 "and GI reaches Complete there", the atmosphere landed, and the seam was never
# joined — `src/rendering/gi/` links `cy::rendering-denoise` and `cy::rendering-raytracing` and not
# `cy::rendering-sky`, and until this module existed NOTHING IN THE TREE CONSTRUCTED A
# `gi::SkyTerm` FROM AN ATMOSPHERE.
#
# EXACTLY ONE LIBRARY IN THIS TREE DEPENDS ON cy::rendering-gi AND cy::rendering-sky AT ONCE, AND IT
# IS THIS ONE. That is the fact the criterion checks, and the two dependency lists below are why it
# had to be a third module rather than an edge added to either of them:
#
#   cy::rendering-sky   src/rendering/sky/CMakeLists.txt says in capitals that it does NOT depend on
#                       cy::rendering-gi, "because a sky that depended on a global illumination
#                       system could not be tested without one" — and `fit_sky_gradient`'s 12.4%
#                       agreement with the atmosphere's own irradiance integral is measured in that
#                       module's suite with no GI present, which is what makes "sufficient for GI's
#                       sky term" a measurement rather than a claim.
#   cy::rendering-gi    links two modules and runs every illumination case headless. Making it link
#                       the sky would drag cy::environment, cy::world, cy::rendering-post,
#                       cy::rendering-temporal and cy::rendering-arbiter behind it, into a module
#                       whose README's last section is "What it does not depend on".
#   cy::environment     THE CLOUD SHADOW FIELD, read through `sky::CloudShadowField::sample` — the
#                       static function terrain, foliage and water call.
#                       `atmosphere-sky-and-clouds` requires the coarse field to be "consumed by
#                       terrain, foliage, water, AND ILLUMINATION"; this module is the fourth.
#   cy::world           the field is sampled at a `world::WorldVec3d`, which is the coordinate the
#                       substrate takes.
#
# NO DEVICE, NO GRAPH, NO SHADER, as with both of the modules it composes. Every case in tests/ runs
# headless, which is the property that lets the seam be judged on a machine with no GPU.
cy_add_module(
    NAME cy_rendering_sky_illumination
    LAYER rendering
    ALIAS cy::rendering-sky-illumination
    TYPE STATIC
    SOURCES
        src/sky_illumination.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-math cy::environment cy::rendering-gi
                        cy::rendering-sky cy::world)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# src/weather/ — CyberWeather: the producer the field substrate was waiting for. M10 tasks 3.1 and
# 3.2, and `weather-and-wind` reaching Working.
#
# ================================================================================================
# THE DEPENDENCY LIST IS THE SPECIFICATION'S FIRST REQUIREMENT, MADE INTO A BUILD FACT
# ================================================================================================
#
# `weather-and-wind` — "Weather publishes state": weather "SHALL influence the world by WRITING
# ENVIRONMENT FIELDS. It SHALL NOT iterate materials, foliage instances, water bodies, or particle
# systems to apply itself." And under "Forbidden environment patterns": "Weather directly mutating
# materials, foliage instances, water bodies, or particle systems" SHALL NOT appear, "and each
# SHALL be checkable".
#
# So it is checked HERE, by the link graph, which is the level at which a forbidden call is
# prevented rather than discouraged:
#
#   cy::environment    IS a dependency. It is the whole of weather's output side — nine fields
#                      declared, claimed with `ProducerToken`s and written through `FieldWriter`.
#   cy::world          IS a dependency, for `WorldVec3d`, `PartitionConfig` and nothing else.
#                      Weather cells are NOT world cells (see cells.h); what is borrowed is the
#                      coordinate vocabulary, so that a position handed to weather is the same
#                      position handed to every other subsystem.
#   cy::terrain        IS NOT, and cannot be. Terrain influence — rain shadow, orographic uplift,
#                      channelling, ridge acceleration — needs one number from terrain, the ground
#                      elevation under a point, and takes it through `TerrainProfile` in cells.h:
#                      a callback the application installs. src/water/ draws the same seam for the
#                      same reason and calls it `BedSource`.
#   cy::foliage,       ARE NOT, and a single line of this module naming one of them would be the
#   cy::vfx,           forbidden pattern itself. They are CONSUMERS: they open a `FieldReader` on
#   cy::water,         `wind` and read it. "A new system that reacts to wind requires no change to
#   cy::scene          weather" is true because there is nothing here for such a change to edit.
#   cy::rendering-*    IS NOT. `atmosphere-sky-and-clouds` declares `sky::CloudWeatherState` and
#                      says in its own header that `weather-and-wind` PRODUCES it — and this module
#                      produces it as `CloudDrive` (see system.h), six numbers the composition point
#                      assigns field by field. A weather module that linked the renderer to name one
#                      struct would be a weather module the cooker cannot run, and would put the
#                      presentation layer underneath the authoritative one.
#
# LAYER. `scene` (4): the rung src/world/, src/environment/, src/water/, src/terrain/,
# src/gameplay/ and src/navigation/ all chose, and for the reason each of them gives — there is no
# `weather` rung in `engine-architecture`'s table, and inventing one is a change to
# cmake/module.cmake that src/gameplay/CMakeLists.txt already declined to make. Read after
# src/environment/, whose fields this module produces, and before every consumer of them.
#
# NO OPTION. `weather-and-wind` gives no removal switch, and the rows that read its fields read
# them through the substrate rather than through this target, so there is no build in which
# removing it is coherent while those exist. cmake/features.cmake is unchanged by this module,
# deliberately.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_weather
    LAYER scene
    ALIAS cy::weather
    TYPE STATIC
    SOURCES
        src/accumulation.cpp
        src/cells.cpp
        src/climate.cpp
        src/diagnostics.cpp
        src/ecosystem.cpp
        src/fields.cpp
        src/precipitation.cpp
        src/preset.cpp
        src/sample.cpp
        src/storm.cpp
        src/system.cpp
        src/wind.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::environment cy::world)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

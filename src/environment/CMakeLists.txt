# src/environment/ — CyberField: the shared environment substrate. M10 section 1, and
# `environment-fields` reaching Working.
#
# ================================================================================================
# WHY THIS IS ITS OWN DIRECTORY AND NOT PART OF TERRAIN
# ================================================================================================
#
# `environment-fields` states it as the capability's purpose rather than as a preference: fields are
# specified "beneath terrain, foliage, and water rather than inside any of them, and that is the
# whole point. Putting moisture inside terrain would make foliage depend on terrain to know whether
# the ground is wet; putting wind inside foliage would make water depend on foliage. Each dependency
# is defensible alone and the set of them is a knot."
#
# So the dependency list below is the requirement made checkable. There is no cy::terrain here, no
# cy::water, no cy::rendering and no device. Six M10 rows write into this module and this module
# names none of them — which is what makes "a consumer that writes a field is impossible rather than
# discouraged" a property of the link graph.
#
# LAYER. `scene` (4): the rung src/world/, src/gameplay/, src/navigation/, src/ai/, src/audio/ and
# src/camera/ all chose, and for the same reason each of them gives — there is no `environment` rung
# in `engine-architecture`'s table and inventing one would be a change to cmake/module.cmake that
# src/gameplay/CMakeLists.txt already declined to make. Layer 4 is also the highest rung any
# consumer sits on, so every consumer the specification names — terrain materials, foliage, water,
# VFX, audio, AI, navigation, gameplay — can link this and none of them has to be moved.
#
# WHY cy::world IS A DEPENDENCY AND cy::environment IS NOT ONE OF ITS. A field is addressed by WORLD
# POSITION, and the runtime's world position is `world::WorldPosition` — cell-relative, for the
# precision reason `world/coordinates.h` gives. Converting it is the partition's arithmetic, and a
# second copy of that arithmetic here would be a second answer to "where is this". The direction is
# one way: nothing under src/world/ names `cy/environment/`, and the one thing M10 added to that
# module is `Channel::Fields` — an enumerator, so that field tiles stream as a cell channel the way
# the specification says they do.
#
# WHY cy::servers-residency IS A DEPENDENCY. "Residency SHALL be requestable ... through the shared
# residency policy in `residency`, and SHALL follow the same importance and budget model as other
# paged data." A module that scored and evicted its own tiles would be the parallel mechanism that
# sentence forbids. See src/environment/include/cy/environment/streaming.h for the page-identifier
# split this module shares with world cells, and for why it registers no budget of its own.
#
# WHY cy::core-determinism IS A DEPENDENCY. design.md §3: "M10 adds no second mechanism." A field's
# classification IS `determinism::SimulationClass` and the firewall's direction IS
# `determinism::may_read()`. Linking that module is what makes the reuse a build fact rather than a
# claim in a comment.
#
# NO OPTION. `environment-fields` gives no removal switch, and unlike CY_UI or CY_VFX there is
# nothing coherent to remove: seven M10 rows sample this substrate, so an engine built without it
# would be an engine without terrain, water, foliage, weather or sky. cmake/features.cmake is
# therefore unchanged by this module, deliberately.

# Sources are listed explicitly and never globbed: a file this list does not name is compiled by
# nothing, which reads in a review exactly like a file that passes.
cy_add_module(
    NAME cy_environment
    LAYER scene
    ALIAS cy::environment
    TYPE STATIC
    SOURCES
        src/field.cpp
        src/gpu.cpp
        src/store.cpp
        src/streaming.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::servers-residency cy::world)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

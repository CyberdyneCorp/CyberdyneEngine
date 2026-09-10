# src/gameplay/abilities/ — layer 4: CyberAbilities. Attributes, effects, abilities, targeting and
# the activation pipeline. M8.b section 4.
#
# ================================================================================================
# A SEPARATE TARGET IS THE REQUIREMENT, NOT A TIDINESS DECISION
# ================================================================================================
#
# `gameplay-abilities-and-effects`, first requirement: "A project that does not use abilities SHALL
# link **none** of this capability's code and SHALL carry **none** of its data." A target nothing
# links is the only form in which that is structurally true rather than believed: `cy::gameplay`
# does not depend on this, so a dedicated server, a headless test or a game with no abilities links
# no attribute store, no effect array and no activation pipeline.
#
# `src/gameplay/tests/test_bypass.cpp` already asserts the converse of this shape for the input
# server, with `__has_include`. `tests/test_abilities.cpp` states the same property from this side:
# nothing outside this directory names `cy::gameplay-abilities`, and adding it to `cy_gameplay`'s
# dependency list would put these headers on the base module's include path — which is exactly what
# "pays nothing" forbids.
#
# WHAT IT DEPENDS ON, AND WHY EACH:
#
#   cy::gameplay   layer 4 — tags, teams, validation reasons, the command vocabulary and the
#                            session's random streams. The framework this module is built ON,
#                            because "This capability SHALL build on the framework's existing
#                            mechanisms ... rather than defining parallel ones."
#   cy::graph      layer 4 — `cy::graph::script::AbilityProgram`. M8.b's spike found that abilities
#                            and visual scripting are ONE language serving two consumers, so the
#                            compiler is CyberGraph's and this module holds what it operates on.
#                            Writing a second compiler here is the duplication the spike measured
#                            against; see include/cy/gameplay/abilities/abilities.h.
#   cy::ecs, cy::core-*      the entity identity, the containers and the determinism primitives.
#
# There is no renderer, no audio and no interface here either. `gameplay-framework`'s "Headless
# operation" is inherited: a cue is a tagged signal in an array, and what draws it lives above.
cy_add_module(
    NAME cy_gameplay_abilities
    LAYER scene
    ALIAS cy::gameplay-abilities
    TYPE STATIC
    SOURCES
        src/abilities.cpp
        src/activation.cpp
        src/attributes.cpp
        src/effects.cpp
        src/targeting.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values cy::ecs cy::gameplay cy::graph
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# src/gameplay/play/ — layer 4: spawning, and the play session that simulates an authored world.
# M8.a section 5.
#
# ================================================================================================
# WHY THIS IS A SEPARATE TARGET FROM cy::gameplay AND NOT THREE MORE FILES IN IT
# ================================================================================================
#
# Read src/gameplay/CMakeLists.txt's own header first: "READ THE DEPENDENCY LIST BEFORE YOU EXTEND
# IT". `cy_gameplay` deliberately depends on `cy::ecs` and the core and NOTHING ELSE — that is what
# makes `tests/test_bypass.cpp`'s `__has_include` check meaningful, and the same property is what
# lets a dedicated server link no rendering code "because there is none to link".
#
# A play session needs three things that module does not have: `cy::scene` for the tree it spawns
# into, `cy::scene-serialization` for the authored `.cyworld` it simulates, and `cy::physics-bridge`
# for the bodies. Adding those to `cy_gameplay` would put four modules' headers on the include path
# of a suite whose whole argument is about what is NOT reachable from it.
#
# So the dependency goes the other way: this target depends on `cy::gameplay` and the module above
# keeps its short list exactly as it was. The bypass invariant is untouched — `cy::servers-input` is
# not here either, and a play session reaching an input device would be the same defect one layer
# up.
#
# WHAT DEPENDS ON WHAT:
#
#   cy::gameplay             layer 4 — the session and command vocabulary a play session belongs to
#   cy::scene                layer 4 — `SceneTree`, `LocalTransform`, transform propagation
#   cy::scene-serialization  layer 4 — `.cyworld`, read and written by the engine (section 1)
#   cy::physics-bridge       layer 4 — the components, the bodies and the `Physics` stage
#   cy::ecs, cy::core-*      below    — the world, the schedule and the clock
#
# There is no `cy::physics-jolt` and no renderer: `gameplay-framework` requires a game to be "fully
# functional with no renderer, no audio, no interface, and no GPU", and a play session is the first
# thing in this engine that could plausibly have wanted one. Its whole suite runs headless.
cy_add_module(
    NAME cy_gameplay_play
    LAYER scene
    ALIAS cy::gameplay-play
    TYPE STATIC
    SOURCES
        src/session.cpp
        src/spawn.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values cy::ecs cy::gameplay cy::physics-bridge cy::scene
                        cy::scene-serialization
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

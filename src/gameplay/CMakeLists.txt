# src/gameplay/ — CyberGameplay: lifetimes, control, and the one validated command stream.
# Section 4.4 of the M4 tasks.
#
# ================================================================================================
# READ THE DEPENDENCY LIST BEFORE YOU EXTEND IT
# ================================================================================================
#
# `cy::servers-input` IS NOT THERE AND MUST NOT BE ADDED. design.md §3: "Input reaches simulation
# **only** as commands. There is no 'read the input state in a system' path." Because this module
# declares no dependency on the input server, its headers are not on a gameplay translation unit's
# include path at all — a gameplay system cannot include one even by accident.
#
# src/gameplay/tests/test_bypass.cpp asserts exactly that, with `__has_include`, and it fails the
# moment the dependency is added. That test also demonstrates *why*: it runs a conforming simulation
# and a bypassing one, replays both from the command log, and shows the bypassing one diverging.
# design.md §3 again: "a single system that reads a device directly ... makes the M9 guarantees
# unachievable until someone finds and removes it, and nothing will point at it, because everything
# will appear to work until a desync months later."
#
# The bridge from input actions to gameplay commands therefore lives **above** both modules — in the
# runtime or in the sample — where it can see the input server and the command stream at once. That
# is the whole architectural content of the invariant: the translation is a thing somebody wrote, in
# one place, rather than a capability every system quietly has.
#
# LAYER. `scene` (4): above cy::ecs (1) and the servers (2), below cy::runtime (5). There is no
# `gameplay` layer in `engine-architecture`'s table and inventing one would be a change to
# cmake/modules.cmake, which is not this milestone's to make; `scene` is the correct existing rung —
# it is where the modules that compose ECS data into game-facing structure already sit.
#
# HEADLESS IS A REQUIREMENT, NOT A CONFIGURATION. `gameplay-framework`: "fully functional with no
# renderer, no audio, no interface, and no GPU ... a gameplay system that requires a camera,
# viewport, material, or audio device SHALL be a defect." None of those is reachable from this
# dependency list — and that sentence used to be the whole of the enforcement, which is to say
# there was none. `cmake/headless.cmake` now walks the TRANSITIVE link closure at configure time and
# refuses the build when one becomes reachable, because a requirement guarded by a comment holds
# only until the next person in a hurry.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/headless.cmake")

cy_add_module(
    NAME cy_gameplay
    LAYER scene
    ALIAS cy::gameplay
    TYPE STATIC
    SOURCES
        src/command.cpp
        src/context.cpp
        src/control.cpp
        src/cook_firewall.cpp
        src/diagnostics.cpp
        src/events.cpp
        src/features.cpp
        src/firewall_arming.cpp
        src/fragments.cpp
        src/indexes.cpp
        src/interaction.cpp
        src/ownership.cpp
        src/rules.cpp
        src/tags.cpp
        src/teams.cpp
        src/time.cpp
        src/validation.cpp
    PUBLIC_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PUBLIC_DEPENDENCIES cy::core-base cy::core-determinism cy::core-math cy::core-memory
                        cy::core-values cy::ecs
    PRIVATE_DEPENDENCIES cy::core-diagnostics)

# M9 task 2.2. See src/core/determinism/CMakeLists.txt for the argument, and
# src/core/determinism/cmake/determinism_profile.cmake for what deleting this line does to the
# configure. Gameplay is covered because the command stream's ordering and the validation that
# precedes it are authoritative simulation: a contracted multiply-add inside a rule is a rule that
# accepts a command on one machine and rejects it on another.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(cy_gameplay PRIVATE -ffp-contract=off)
elseif(MSVC)
    target_compile_options(cy_gameplay PRIVATE /fp:precise)
endif()

cy_declare_determinism_profile(cy_gameplay
    PROFILE SamePlatform
    REASON "the command stream and its validation are authoritative; CrossPlatform would need deterministic math types, which this tree does not have")

if(CY_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()

# abilities/ — CyberAbilities, M8.b section 4. A SEPARATE TARGET because
# `gameplay-abilities-and-effects` opens with "A project that does not use abilities SHALL link none
# of this capability's code and SHALL carry none of its data", and a target nothing links is the
# only form in which that is structurally true. It depends on cy::gameplay and cy::graph; the list
# above is unchanged, so tests/test_bypass.cpp's argument about what is NOT reachable from here
# still holds. See abilities/CMakeLists.txt.
add_subdirectory(abilities)

# play/ — spawning and the play session, M8.a section 5. A SEPARATE TARGET on purpose: it needs
# cy::scene, cy::scene-serialization and cy::physics-bridge, and putting those on THIS target's
# dependency list would put four modules' headers on tests/test_bypass.cpp's include path, whose
# whole argument is about what is not reachable from here. It depends on cy::gameplay rather than
# the other way round, so the list above is unchanged. See play/CMakeLists.txt.
add_subdirectory(play)

# The headless check, deferred to the end of the top-level directory: a target's transitive link
# closure is only complete once every module in the tree has been declared, so running it here in
# directory order would check a closure that is not yet the closure.
#
# `cy_gameplay_play` is checked as well as `cy_gameplay`: it is the target a dedicated server links
# to spawn and run a session, so it is the one the requirement's "the dedicated server SHALL link no
# rendering code" is actually about. `cy_gameplay_abilities` too — abilities are simulation.
function(cy_gameplay_check_headless)
    cy_require_headless(cy_gameplay)
    cy_require_headless(cy_gameplay_abilities)
    cy_require_headless(cy_gameplay_play)
endfunction()

cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL cy_gameplay_check_headless)
